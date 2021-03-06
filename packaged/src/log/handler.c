/* -*- coding: utf-8 -*-
 ###############################################################################
 # Author: Pierre Vigneras <pierre.vigneras@bull.net>
 # Created on: May 24, 2013
 # Contributors:
 ###############################################################################
 # Copyright (C) 2012  Bull S. A. S.  -  All rights reserved
 # Bull, Rue Jean Jaures, B.P.68, 78340, Les Clayes-sous-Bois
 # This is not Free or Open Source software.
 # Please contact Bull S. A. S. for details about its license.
 ###############################################################################
 */


#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sysexits.h>
#include <string.h>

#include "bxi/base/err.h"
#include "bxi/base/mem.h"
#include "bxi/base/str.h"
#include "bxi/base/time.h"
#include "bxi/base/zmq.h"

#include "bxi/base/log/handler.h"
#include "bxi/base/log/filter.h"

#include "handler_impl.h"
#include "log_impl.h"


//*********************************************************************************
//********************************** Defines **************************************
//*********************************************************************************


//*********************************************************************************
//********************************** Types ****************************************
//*********************************************************************************

typedef struct {
    void * ctrl_zocket;
    void * data_zocket;

#ifdef __linux__
    pid_t tid;                              // the thread pid
#endif
} handler_data_s;

typedef handler_data_s * handler_data_p;

//*********************************************************************************
//********************************** Static Functions  ****************************
//*********************************************************************************
static bxierr_p _mask_signals(bxilog_handler_p);
static bxierr_p _create_zockets(bxilog_handler_p,
                                bxilog_handler_param_p,
                                handler_data_p);
static bxierr_p _bind_ctrl_zocket(bxilog_handler_p,
                                  bxilog_handler_param_p,
                                  handler_data_p);
static bxierr_p _bind_data_zocket(bxilog_handler_p,
                                  bxilog_handler_param_p,
                                  handler_data_p);
static bxierr_p _init_handler(bxilog_handler_p,
                              bxilog_handler_param_p,
                              handler_data_p);
static bxierr_p _send_ready_status(bxilog_handler_p,
                                   bxilog_handler_param_p,
                                   handler_data_p, bxierr_p err);
static bxierr_p _loop(bxilog_handler_p,
                      bxilog_handler_param_p,
                      handler_data_p);
static bxierr_p _cleanup(bxilog_handler_p,
                         bxilog_handler_param_p,
                         handler_data_p);
static bxierr_p _internal_flush(bxilog_handler_p,
                                bxilog_handler_param_p,
                                handler_data_p);
static bxierr_p _process_ierr(bxilog_handler_p handler,
                              bxilog_handler_param_p,
                              bxierr_p err);
static bxierr_p _process_log_record(bxilog_handler_p,
                                    bxilog_handler_param_p,
                                    handler_data_p);
static bxierr_p _process_log_zmsg(bxilog_handler_p handler,
                                  bxilog_handler_param_p param,
                                  handler_data_p data, zmq_msg_t zmsg);
static bxierr_p _process_ctrl_cmd(bxilog_handler_p,
                                  bxilog_handler_param_p,
                                  handler_data_p);
static bxierr_p _process_implicit_flush(bxilog_handler_p,
                                        bxilog_handler_param_p,
                                        handler_data_p);
static bxierr_p _process_explicit_flush(bxilog_handler_p,
                                        bxilog_handler_param_p,
                                        handler_data_p);
static bxierr_p _process_exit(bxilog_handler_p,
                              bxilog_handler_param_p,
                              handler_data_p);

//*********************************************************************************
//********************************** Global Variables  ****************************
//*********************************************************************************

//*********************************************************************************
//********************************** Implementation    ****************************
//*********************************************************************************

void bxilog_handler_init_param(bxilog_handler_p handler,
                               bxilog_filters_p filters,
                               bxilog_handler_param_p param) {
    bxiassert(NULL != param);

    param->data_hwm = 1000;
    param->ctrl_hwm = 1000;
    param->flush_freq_ms = 1000;
    param->ierr_max = 10;
    param->filters = filters;

    // Use the param pointer to guarantee a unique URL name for different instances of
    // the same handler
    param->ctrl_url = bxistr_new("inproc://%s/%p.ctrl", handler->name, param);
    param->data_url = bxistr_new("inproc://%s/%p.data", handler->name, param);
}

void bxilog_handler_clean_param(bxilog_handler_param_p param) {
    BXIFREE(param->ctrl_url);
    BXIFREE(param->data_url);
    bxilog_filters_destroy(&param->filters);
    // Do not free param since it has not been allocated by init()
    // BXIFREE(param);
 }

bxierr_p bxilog__handler_start(bxilog__handler_thread_bundle_p bundle) {
    bxilog_handler_p handler = bundle->handler;
    bxilog_handler_param_p param = bundle->param;

    BXIFREE(bundle);

    bxierr_p eerr = BXIERR_OK, eerr2; // External errors
    bxierr_p ierr = BXIERR_OK;        // Internal errors
    handler_data_s data;
    memset(&data, 0, sizeof(data));

    // Constants for the IHT
#ifdef __linux__
    data.tid = (pid_t) syscall(SYS_gettid);
#endif

    eerr2 = _init_handler(handler, param, &data);
    BXIERR_CHAIN(eerr, eerr2);
    // Do not quit immediately, we need to send a ready message to BC.

    ierr = _create_zockets(handler, param, &data);
    eerr2 = _process_ierr(handler, param, ierr);
    BXIERR_CHAIN(eerr, eerr2);

    ierr = _mask_signals(handler);
    eerr2 = _process_ierr(handler, param, ierr);
    BXIERR_CHAIN(eerr, eerr2);

    ierr = _send_ready_status(handler, param, &data, eerr);
    eerr2 = _process_ierr(handler, param, ierr);
    BXIERR_CHAIN(eerr, eerr2);

    // Ok, we synced with BC. If now there is an error at that stage
    // no need to go in the loop, just cleanup and exit.
    if (bxierr_isko(eerr)) goto CLEANUP;

    ierr = _loop(handler, param, &data);
    eerr2 = _process_ierr(handler, param, ierr);
    BXIERR_CHAIN(eerr, eerr2);

CLEANUP:
    ierr = _cleanup(handler, param, &data);
    eerr2 = _process_ierr(handler, param, ierr);
    BXIERR_CHAIN(eerr, eerr2);

    eerr2 = _process_exit(handler, param, &data);
    BXIERR_CHAIN(eerr, eerr2);

    return eerr;
}

//*********************************************************************************
//********************************** Static Helpers Implementation ****************
//*********************************************************************************

// All signals are blocked in this thread, they must be dealt with
// by other threads. Note: this is only true for asynchronous signals
// such as SIGINT, SIGQUIT and so on...
// In particular synchronous signals such as SIGSEGV, SIGBUS, and the like
// are always sent to the thread that generated them. Therefore,
// there is no real thing to do unless implementing a complex sigsetjmp/siglongjmp
// in order to produce a log, and flush the log.
// However, this is complex too, since signal handlers is per process, not per-thread
// Therefore, all threads will share same signal handler.
bxierr_p _mask_signals(bxilog_handler_p handler) {
    bxierr_p err = BXIERR_OK, err2 = BXIERR_OK;
    sigset_t mask;
    int rc = sigfillset(&mask);
    if (0 != rc) err2 = bxierr_errno("%s: calling sigfillset() failed", handler->name);
    BXIERR_CHAIN(err, err2);

    // block all signals.
    // Note: undefined behaviour for SIGBUS, SIGFPE, SIGILL and SIGSEGV
    // See sigprocmask(2)
    rc = pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (-1 == rc) err2 = bxierr_errno("%s: calling pthread_sigmask() failed",
                                      handler->name);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _create_zockets(bxilog_handler_p handler,
                         bxilog_handler_param_p param,
                         handler_data_p data) {

    bxierr_p err = BXIERR_OK, err2;

    err2 = _bind_ctrl_zocket(handler, param, data);
    BXIERR_CHAIN(err, err2);

    err2 = _bind_data_zocket(handler, param, data);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _bind_ctrl_zocket(bxilog_handler_p handler,
                           bxilog_handler_param_p param,
                           handler_data_p data) {
    UNUSED(handler);
    bxierr_p err = BXIERR_OK, err2;

    bxiassert(NULL == data->ctrl_zocket);

    err2 = bxizmq_zocket_create(BXILOG__GLOBALS->zmq_ctx,
                                ZMQ_REP,
                                &data->ctrl_zocket);
    BXIERR_CHAIN(err, err2);

    err2 = bxizmq_zocket_setopt(data->ctrl_zocket,
                                ZMQ_RCVHWM,
                                &param->ctrl_hwm,
                                sizeof(param->ctrl_hwm));
    BXIERR_CHAIN(err, err2);

    int affected_port;
    err2 = bxizmq_zocket_bind(data->ctrl_zocket,
                              param->ctrl_url,
                              &affected_port);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _bind_data_zocket(bxilog_handler_p handler,
                           bxilog_handler_param_p param,
                           handler_data_p data) {
    UNUSED(handler);
    bxierr_p err = BXIERR_OK, err2;

    bxiassert(NULL == data->data_zocket);

    err2 = bxizmq_zocket_create(BXILOG__GLOBALS->zmq_ctx,
                                ZMQ_DEALER,
                                &data->data_zocket);
    BXIERR_CHAIN(err, err2);

    err2 = bxizmq_zocket_setopt(data->data_zocket,
                                ZMQ_IDENTITY,
                                &param->rank,
                                sizeof(param->rank));
    BXIERR_CHAIN(err, err2);

    err2 = bxizmq_zocket_setopt(data->data_zocket,
                                ZMQ_RCVHWM,
                                &param->data_hwm,
                                sizeof(param->data_hwm));
    BXIERR_CHAIN(err, err2);

    int affected_port;

    err2 = bxizmq_zocket_bind(data->data_zocket,
                              param->data_url,
                              &affected_port);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _init_handler(bxilog_handler_p handler,
                       bxilog_handler_param_p param,
                       handler_data_p data) {
    UNUSED(data);
    return (NULL == handler->init) ? BXIERR_OK : handler->init(param);
}

bxierr_p _send_ready_status(bxilog_handler_p handler,
                            bxilog_handler_param_p param,
                            handler_data_p data,
                            bxierr_p err) {
    UNUSED(handler);
    UNUSED(param);

    char * msg;
    bxierr_p fatal_err = bxizmq_str_rcv(data->ctrl_zocket, 0, 0, &msg);
    bxierr_abort_ifko(fatal_err);

    if (0 != strncmp(READY_CTRL_MSG_REQ, msg, ARRAYLEN(READY_CTRL_MSG_REQ))) {
        bxierr_p result = bxierr_new(BXIZMQ_PROTOCOL_ERR, NULL, NULL, NULL, NULL,
                                     "Expected message '%s' but received '%s'",
                                     READY_CTRL_MSG_REQ, msg);
        BXIFREE(msg);
        return result;
    }

    BXIFREE(msg);
    if (bxierr_isok(err)) {
        fatal_err = bxizmq_str_snd(READY_CTRL_MSG_REP, data->ctrl_zocket,
                                   ZMQ_SNDMORE, 0, 0);
        BXIERR_CHAIN(err, fatal_err);
        bxierr_abort_ifko(fatal_err);

        fatal_err = bxizmq_data_snd(&param->rank, sizeof(param->rank), data->ctrl_zocket,
                                    0, 0, 0);
        BXIERR_CHAIN(err, fatal_err);
        bxierr_abort_ifko(fatal_err);

        return err;
    }

    // Send the error message
    char * err_str = bxierr_str(err);
    fatal_err = bxizmq_str_snd(err_str, data->ctrl_zocket, ZMQ_SNDMORE, 0, 0);
    BXIFREE(err_str);
    bxierr_abort_ifko(fatal_err);
    fatal_err = bxizmq_data_snd(&param->rank, sizeof(param->rank), data->ctrl_zocket,
                                0, 0, 0);
    bxierr_abort_ifko(fatal_err);

    return BXIERR_OK;
}

bxierr_p _loop(bxilog_handler_p handler,
               bxilog_handler_param_p param,
               handler_data_p data) {

    bxierr_p err = BXIERR_OK, err2;

    size_t items_nb = 2 + param->private_items_nb;
    zmq_pollitem_t items[items_nb];
    items[0].socket = data->ctrl_zocket;
    items[0].events = ZMQ_POLLIN;
    items[1].socket = data->data_zocket;
    items[1].events = ZMQ_POLLIN;
    for (size_t i = 0; i < param->private_items_nb; i++) {
        memcpy(items + 2 + i, param->private_items + i, sizeof(items[2+i]));
    }


    long actual_timeout = param->flush_freq_ms;
    struct timespec last_flush_time;
    err2 = bxitime_get(CLOCK_MONOTONIC_RAW, &last_flush_time);
    if (bxierr_isko(err2)) bxierr_report(&err2, STDERR_FILENO);

    while (true) {
        errno = 0;
        int rc = zmq_poll(items, (int) items_nb, actual_timeout);

        if (-1 == rc) {
            if (EINTR == errno) continue; // One interruption happened
                                          // (e.g. with profiling)

            int code = zmq_errno();
            const char * msg = zmq_strerror(code);
            bxierr_p ierr = bxierr_new(code, NULL, NULL, NULL, NULL,
                                       "Calling zmq_poll() failed: %s", msg);
            err2 = _process_implicit_flush(handler, param, data);
            BXIERR_CHAIN(err, err2);
            err2 = _process_ierr(handler, param, ierr);
            BXIERR_CHAIN(err, err2);
            if (bxierr_isko(err)) goto QUIT;
        }
        double tmp;
        err2 = bxitime_duration(CLOCK_MONOTONIC_RAW, last_flush_time, &tmp);
        if (bxierr_isko(err2)) bxierr_report(&err2, STDERR_FILENO);
        long duration_since_last_flush = (long) (tmp * 1e3);

        actual_timeout = param->flush_freq_ms - duration_since_last_flush;

//        fprintf(stderr,
//                "Duration: %ld, Actual Timeout:  %ld\n",
//                duration_since_last_flush, actual_timeout);

        if (0 == rc || 0 >= actual_timeout) {
            // 0 == rc: nothing to poll -> do a flush() and start again
            // 0 >= actual_timeout:
            // we might have received billions of logs that were filtered out
            // and very few of them have been filtered in (accepted by the handler)
            // however, if we do not flush, those accepted messages might never
            // be seen unless an explicit flush is requested. As an example,
            // if the handler (file_handler) buffers messages, only few of them have
            // reached the buffer in let say 10 minutes which is therefore almost
            // empty and not written to the underlying storage. For the end user
            // it seems therefore that nothing happened at all! So we must guarantee
            // implicit flush is requested at regular interval

//            fprintf(stderr, "Implicit flush\n");
            err2 = _process_implicit_flush(handler, param, data);
            BXIERR_CHAIN(err, err2);

            err2 = bxitime_get(CLOCK_MONOTONIC_RAW, &last_flush_time);
            if (bxierr_isko(err2)) bxierr_report(&err2, STDERR_FILENO);
            actual_timeout = param->flush_freq_ms;

            err = _process_ierr(handler, param, err);
            if (bxierr_isko(err)) goto QUIT;
            continue;
        }
        if (items[0].revents & ZMQ_POLLIN) {
            // Process ctrl message
            err2 = _process_ctrl_cmd(handler, param, data);
            BXIERR_CHAIN(err, err2);
            // Treat explicit exit message
            if (BXILOG_HANDLER_EXIT_CODE == err->code) goto QUIT;
            err = _process_ierr(handler, param, err);
            if (bxierr_isko(err)) goto QUIT;
        }
        if (items[1].revents & ZMQ_POLLIN) {
            // Process data, this is the normal case
            err2 = _process_log_record(handler, param, data);

            if (EAGAIN == err2->code) {
                // Might happened on interruption!
                bxierr_destroy(&err2);
                err2 = BXIERR_OK;
            }
            BXIERR_CHAIN(err, err2);

            err = _process_ierr(handler, param, err);
            if (bxierr_isko(err)) goto QUIT;
        }
        for (size_t i = 0; i < param->private_items_nb; i++) {
            if (0 != items[2+i].revents) {
                if (NULL != param->cbs[i]) {
                    err2 = param->cbs[i](param, items[2+i].revents);
                    if (bxierr_isko(err)) goto QUIT;
                }
            }
        }
    }
QUIT:

    return err;
}

bxierr_p _cleanup(bxilog_handler_p handler,
                  bxilog_handler_param_p param,
                  handler_data_p data) {
    UNUSED(handler);
    UNUSED(param);

    bxierr_p err = BXIERR_OK, err2;

    err2 =  bxizmq_zocket_destroy(&data->data_zocket);
    BXIERR_CHAIN(err, err2);

    err2 = bxizmq_zocket_destroy(&data->ctrl_zocket);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _internal_flush(bxilog_handler_p handler,
                         bxilog_handler_param_p param,
                         handler_data_p data) {


    bxierr_p err = BXIERR_OK;
    while(true) {
        err = _process_log_record(handler, param, data);
        if (bxierr_isko(err)) break;
    }
    if (EAGAIN == err->code) {
        bxierr_destroy(&err);
        err = BXIERR_OK;
    }

    return err;
}

bxierr_p _process_log_record(bxilog_handler_p handler,
                             bxilog_handler_param_p param,
                             handler_data_p data) {
    zmq_msg_t zmsg;
    errno = 0;
    int rc = zmq_msg_init(&zmsg);
    bxiassert(0 == rc);

    bxierr_p err = BXIERR_OK, err2;

    err2 = bxizmq_msg_rcv(data->data_zocket, &zmsg, ZMQ_DONTWAIT);
    BXIERR_CHAIN(err, err2);

    if (bxierr_isko(err)) {
        err2 = bxizmq_msg_close(&zmsg);
        BXIERR_CHAIN(err, err2);
        return err;
    }

    err2 = _process_log_zmsg(handler, param, data, zmsg);
    BXIERR_CHAIN(err, err2);
    /* Release */
    err2 = bxizmq_msg_close(&zmsg);
    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _process_log_zmsg(bxilog_handler_p handler,
                           bxilog_handler_param_p param,
                           handler_data_p data,
                           zmq_msg_t zmsg) {


    // bxiassert we received enough data
//    const size_t received_size = zmq_msg_size(&zmsg);
//    bxiassert(received_size >= BXILOG__GLOBALS->RECORD_MINIMUM_SIZE);

    bxilog_record_s * record = zmq_msg_data(&zmsg);

    // Fetch other strings: filename, funcname, loggername, logmsg
    char * filename = (char *) record + sizeof(*record);
    char * funcname = filename + record->filename_len;
    char * loggername = funcname + record->funcname_len;
    char * logmsg = loggername + record->logname_len;

    bxilog_level_e filter_level = BXILOG_OFF;

    for (size_t i = 0; i < param->filters->nb; i++) {
        bxilog_filter_p filter = param->filters->list[i];
        if (NULL == filter) break;

        if (0 == strncmp(filter->prefix, loggername, strlen(filter->prefix))) {
            filter_level = filter->level;
//            fprintf(stderr, "%d.%d: %s MATCH [%zu] %s:%d\n",
//                                    record->pid, record->tid, loggername,
//                                    i, filter->prefix, filter->level);
        } else {
//            fprintf(stderr, "%d.%d: %s MISMATCH [%zu] %s:%d\n",
//                                                record->pid, record->tid, loggername,
//                                                i, filter->prefix, filter->level);
        }
    }
    bxierr_p err = BXIERR_OK;
    if ((record->level <= filter_level) && (NULL != handler->process_log)) {
            err = handler->process_log(record,
                                       filename, funcname, loggername, logmsg,
                                       param);
    } else {
        UNUSED(data);
    }

    return err;
}

bxierr_p _process_ctrl_cmd(bxilog_handler_p handler,
                           bxilog_handler_param_p param,
                           handler_data_p data) {

    bxierr_p err = BXIERR_OK, err2;

    char * cmd = NULL;
    err2 = bxizmq_str_rcv(data->ctrl_zocket, ZMQ_DONTWAIT, false, &cmd);
    BXIERR_CHAIN(err, err2);
    if (bxierr_isko(err)) {
        // Nothing to process, this might happened if a flush has been asked
        // and processed before this function call
        bxiassert(NULL == cmd);
        if (EAGAIN == err->code) {
            bxierr_destroy(&err);
            return BXIERR_OK;
        }
        return err;
    }

    if (0 == strncmp(READY_CTRL_MSG_REQ, cmd, ARRAYLEN(READY_CTRL_MSG_REQ))) {
        BXIFREE(cmd);

        err2 = bxizmq_str_snd(READY_CTRL_MSG_REP, data->ctrl_zocket, ZMQ_SNDMORE, 0, 0);
        BXIERR_CHAIN(err, err2);

        err2 = bxizmq_data_snd(&param->rank, sizeof(param->rank), data->ctrl_zocket, 0, 0, 0);
        BXIERR_CHAIN(err, err2);

        return err;
    }

    if (0 == strncmp(FLUSH_CTRL_MSG_REQ, cmd, ARRAYLEN(FLUSH_CTRL_MSG_REQ))) {
        BXIFREE(cmd);
        err2 = _process_explicit_flush(handler, param, data);
        BXIERR_CHAIN(err, err2);
        err2 = bxizmq_str_snd(FLUSH_CTRL_MSG_REP, data->ctrl_zocket, 0, 0, 0);
        BXIERR_CHAIN(err, err2);
        return err;
    }
    if (0 == strncmp(EXIT_CTRL_MSG_REQ, cmd, ARRAYLEN(EXIT_CTRL_MSG_REQ))) {
        BXIFREE(cmd);

        err2 = _process_implicit_flush(handler, param, data);
        BXIERR_CHAIN(err, err2);

        err2 = bxizmq_str_snd(EXIT_CTRL_MSG_REP, data->ctrl_zocket, 0, 0, 0);
        BXIERR_CHAIN(err, err2);

        return bxierr_new(BXILOG_HANDLER_EXIT_CODE, err, NULL, NULL, NULL,
                          "Exit requested");
    }
    err2 = bxierr_gen("%s: unknown control command: %s", handler->name, cmd);
    BXIERR_CHAIN(err, err2);
    BXIFREE(cmd);
    return err;
}

bxierr_p _process_implicit_flush(bxilog_handler_p handler,
                                 bxilog_handler_param_p param,
                                 handler_data_p data) {

    bxierr_p err = BXIERR_OK, err2;

    err2 = _internal_flush(handler, param, data);
    BXIERR_CHAIN(err, err2);

    err2 = (NULL == handler->process_implicit_flush) ? BXIERR_OK :
            handler->process_implicit_flush(param);

    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _process_explicit_flush(bxilog_handler_p handler,
                                 bxilog_handler_param_p param,
                                 handler_data_p data) {

    bxierr_p err = BXIERR_OK, err2;

    err2 = _internal_flush(handler, param, data);
    BXIERR_CHAIN(err, err2);

    err2 = (NULL == handler->process_explicit_flush) ? BXIERR_OK :
            handler->process_explicit_flush(param);

    BXIERR_CHAIN(err, err2);

    return err;
}

bxierr_p _process_exit(bxilog_handler_p handler,
                       bxilog_handler_param_p param,
                       handler_data_p data) {

    UNUSED(handler);
    UNUSED(data);

    return (NULL == handler->process_exit) ?
            BXIERR_OK :
            handler->process_exit(param);
}

bxierr_p _process_ierr(bxilog_handler_p handler,
                       bxilog_handler_param_p param,
                       bxierr_p err) {

    if (bxierr_isok(err) || NULL == handler->process_ierr) return err;

    bxierr_p actual_err = err;
    if (BXILOG_HANDLER_EXIT_CODE == err->code) {
        bxiassert(NULL == err->cause);
        actual_err = (NULL == err->data) ? BXIERR_OK : err->data;
        err->data = NULL;
        bxierr_destroy(&err);
    }

    return handler->process_ierr(&actual_err, param);
}
