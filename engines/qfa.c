/*
 *  QFA engine
 */

#include "../fio.h"
#include "../optgroup.h"
#include "neon_client.h"

//#define DEBUG

struct fio_qfa_iou {
	struct io_u *io_u;
	int io_seen;
	int io_complete;
};

struct qfa_data {
    struct qfa_client_volume* vol;
	struct io_u **aio_events;
	struct io_u **sort_events;
};

struct qfa_options {
	void *pad;
	char *volume_name;
	char *config_file;
	int  busy_poll;
	int use_tcp;
};

static struct fio_option options[] = {
	{
		.name		= "volume",
		.lname		= "qfa engine volume",
		.type		= FIO_OPT_STR_STORE,
		.help		= "Volume name for QFA engine",
		.off1		= offsetof(struct qfa_options, volume_name),
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_QFA,
	},
	{
        .name       = "config_file",
        .lname      = "qfa configure file",
        .type       = FIO_OPT_STR_STORE,
        .help       = "The configure file of QFA store system",
        .off1       = offsetof(struct qfa_options, config_file),
        .category   = FIO_OPT_C_ENGINE,
        .group      = FIO_OPT_G_QFA,
    },
	{
		.name		= "busy_poll",
		.lname		= "Busy poll",
		.type		= FIO_OPT_BOOL,
		.help		= "Busy poll for completions instead of sleeping",
		.off1		= offsetof(struct qfa_options, busy_poll),
		.def		= "0",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_QFA,
	},
	{
		.name = "use_tcp",
		.lname = "Use TCP",
		.type = FIO_OPT_BOOL,
		.help = "Use TCP instead of RDMA",
		.off1 = offsetof(struct qfa_options, use_tcp),
		.def = "0",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_QFA,
	},
	{
		.name = NULL,
	},
};

static int _fio_setup_qfa_data(struct thread_data *td,
                                struct qfa_data **qbd_data_ptr)
{
    struct qfa_data *qbd;

    if (td->io_ops_data)
        return 0;

    qbd = calloc(1, sizeof(struct qfa_data));
    if (!qbd)
        goto failed;

    qbd->aio_events = calloc(td->o.iodepth, sizeof(struct io_u *));
    if (!qbd->aio_events)
        goto failed;

    qbd->sort_events = calloc(td->o.iodepth, sizeof(struct io_u *));
    if (!qbd->sort_events)
        goto failed;

	*qbd_data_ptr = qbd;
    return 0;

failed:
    if (qbd)
        free(qbd);
    return 1;

}

static int _fio_qfa_connect(struct thread_data *td)
{
    struct qfa_data *qbd = td->io_ops_data;
    struct qfa_options *o = td->eo;

    qbd->vol = qfa_open_volume(o->volume_name,o->config_file, NULL, o->use_tcp?TCP:RDMA);
    if(qbd->vol == NULL)
    {
        log_err("qfa open volume[%s]  failed!", o->volume_name);
        return -1;
    }
    return 0;
}

static void _fio_qfa_disconnect(struct qfa_data *qbd)
{
	if (!qbd)
		return;

	/* shutdown everything */
	if (qbd->vol) {
		qfa_close_volume(qbd->vol);
		qbd->vol = NULL;
	}
}

static void _fio_qfa_finish_aiocb(int complete_status, void* data)
{
	struct fio_qfa_iou *fri = data;
	struct io_u *io_u = fri->io_u;

	fri->io_complete = 1;

	if (complete_status != 0) {
		io_u->error = EIO;
		io_u->resid = io_u->xfer_buflen;
	} else {
		#ifdef DEBUG
		printf("success\n");
		fflush(stdout);
		#endif
		io_u->error = 0;
	}
}

static struct io_u *fio_qfa_event(struct thread_data *td, int event)
{
    struct qfa_data *qbd = td->io_ops_data;

    return qbd->aio_events[event];
}

static inline int fri_check_complete(struct qfa_data *qbd, struct io_u *io_u,
                     unsigned int *events)
{
    struct fio_qfa_iou *fri = io_u->engine_data;

    if (fri->io_complete) {
        fri->io_seen = 1;
        qbd->aio_events[*events] = io_u;
        (*events)++;

        return 1;
    }

    return 0;
}

static inline int qfa_io_u_seen(struct io_u *io_u)
{
    struct fio_qfa_iou *fri = io_u->engine_data;

    return fri->io_seen;
}

static int qfa_io_u_cmp(const void *p1, const void *p2)
{
    const struct io_u **a = (const struct io_u **) p1;
    const struct io_u **b = (const struct io_u **) p2;
    uint64_t at, bt;

    at = utime_since_now(&(*a)->start_time);
    bt = utime_since_now(&(*b)->start_time);

    if (at < bt)
        return -1;
    else if (at == bt)
        return 0;
    else
        return 1;
}

static int qfa_iter_events(struct thread_data *td, unsigned int *events,
			   				unsigned int min_evts, int wait)
{
	struct qfa_data *qbd = td->io_ops_data;
	unsigned int this_events = 0;
	struct io_u *io_u;
	int i, sidx;

	sidx = 0;
	io_u_qiter(&td->io_u_all, io_u, i) {
		if (!(io_u->flags & IO_U_F_FLIGHT))
			continue;
		if (qfa_io_u_seen(io_u))
			continue;

		if (fri_check_complete(qbd, io_u, events))
			this_events++;
		else if (wait)
			qbd->sort_events[sidx++] = io_u;
	}

	if (!wait || !sidx)
		return this_events;

	/*
 	 * 	Sort events, oldest issue first, then wait on as many as we
 	 *  need in order of age. If we have enough events, stop waiting,
 	 *  and just check if any of the older ones are done.
 	 */
	if (sidx > 1)
		qsort(qbd->sort_events, sidx, sizeof(struct io_u *), qfa_io_u_cmp);

	for (i = 0; i < sidx; i++) {
		io_u = qbd->sort_events[i];

		if (fri_check_complete(qbd, io_u, events)) {
			this_events++;
			continue;
		}

		/*
 		 * Stop waiting when we have enough, but continue checking
 		 * all pending IOs if they are complete.
 		 */
		if (*events >= min_evts)
			continue;

		if (fri_check_complete(qbd, io_u, events))
			this_events++;
	}

	return this_events;
}

static int fio_qfa_getevents(struct thread_data *td, unsigned int min,
			     unsigned int max, const struct timespec *t)
{
	unsigned int this_events, events = 0;
	struct qfa_options *o = td->eo;
	int wait = 0;
	#ifdef DEBUG
	printf("before events\n");
	fflush(stdout);
	#endif
	do {
		this_events = qfa_iter_events(td, &events, min, wait);

		if (events >= min)
			break;
		if (this_events)
			continue;

		if (!o->busy_poll)
			wait = 1;
		else
			nop;
	} while (1);
	#ifdef DEBUG
	printf("after events\n");
	fflush(stdout);
	#endif
	return events;
}

static int fio_qfa_queue(struct thread_data *td, struct io_u *io_u)
{
	struct qfa_data *qbd = td->io_ops_data;
	struct fio_qfa_iou *fri = io_u->engine_data;
	int r = -1;
	unsigned int read_retry_cnt = 0;
	unsigned int write_retry_cnt = 0;
	#ifdef DEBUG
	printf("before queue\n");
	fflush(stdout);
	#endif
	//gettimeofday(&task_start, NULL);

//	printf("counter=%lld\n", counter++);

	fio_ro_check(td, io_u);

	fri->io_seen = 0;
	fri->io_complete = 0;

RETRY:
	if (io_u->ddir == DDIR_WRITE) {
		//printf("io offset=%llu, len=%lu\n", io_u->offset, io_u->xfer_buflen);
		//fflush(stdout);
		r = qfa_aio_write(qbd->vol, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset>>12, io_u->xfer_buflen>>12,
					  _fio_qfa_finish_aiocb, fri);
		if (r < 0) {
			if(r == -EAGAIN)
			{
				if (++write_retry_cnt >= 6)
				{
					printf("AIO write  got EAGAIN, sleep %d*1ms.\n", write_retry_cnt);
					write_retry_cnt = 0;
				}

				fprintf(stderr, "AIO write  got EAGAIN, sleep %d*1ms.\n", write_retry_cnt);
				usleep(1000);
				goto RETRY;
			}
			goto failed_comp;
		}

	} else if (io_u->ddir == DDIR_READ) {
		r = qfa_aio_read(qbd->vol, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset >> 12, io_u->xfer_buflen >> 12,
            _fio_qfa_finish_aiocb, fri);

		if (r < 0) {
			if(r == -EAGAIN)
			{
				if (++read_retry_cnt >= 6)
				{
					printf("AIO read got EAGAIN, sleep %d*1ms.\n", read_retry_cnt);
					read_retry_cnt = 0;
				}
				fprintf(stderr, "AIO read got EAGAIN, sleep %d*1ms.\n", read_retry_cnt);
				usleep(1000);
				goto RETRY;
			}
			goto failed_comp;
		}
	} else {
		dprint(FD_IO, "%s: Warning: unhandled ddir: %d\n", __func__,
		       io_u->ddir);
		goto failed_comp;
	}
	//gettimeofday(&task_end, NULL);
	//timeuse = (task_end.tv_sec - task_start.tv_sec) * 1000000
 //                 + (task_end.tv_usec - task_start.tv_usec);
	#ifdef DEBUG
	printf("after queue\n");
	fflush(stdout);
	#endif
	//printf("time_use=%f\n", timeuse);
	return FIO_Q_QUEUED;
failed_comp:
	printf("cmpare\n");
	printf("failed\n");
	io_u->error = r;
	td_verror(td, io_u->error, "xfer");
	return FIO_Q_COMPLETED;
}

static int fio_qfa_init(struct thread_data *td)
{
    int r = _fio_qfa_connect(td);
    if (r) {
        log_err("fio_qfa_connect failed, return code: %d .\n", r);
        return 1;
    }
    return 0;
}

static void fio_qfa_cleanup(struct thread_data *td)
{
    struct qfa_data *qbd = td->io_ops_data;

    if (qbd) {
        _fio_qfa_disconnect(qbd);
        free(qbd->aio_events);
        free(qbd->sort_events);
        free(qbd);
    }
}

static int fio_qfa_setup(struct thread_data *td)
{
    struct fio_file *f;
    struct qfa_data *qbd = NULL;
    struct qfa_options *o = td->eo;
    int r;
	struct qfa_volume_info info;
    /* allocate engine specific structure to deal with libs5bd. */
    r = _fio_setup_qfa_data(td, &qbd);
    if (r) {
        log_err("fio_setup_s5bd_data failed.\n");
        goto cleanup;
    }
	td->io_ops_data = qbd;

    td->o.use_thread = 1;

	dprint(FD_IO, "qfa-engine: volume size: %lu\n", qbd->vol->size);

    /* taken from "net" engine. Pretend we deal with files,
     * even if we do not have any ideas about files.
     * The size of the S5BD is set instead of a artificial file.
     */
    if (!td->files_index) {
        add_file(td, td->o.filename ? : "qbd", 0, 0);
        td->o.nr_files = td->o.nr_files ? : 1;
        td->o.open_files++;
    }
    f = td->files[0];

    r = qfa_query_volume(o->volume_name, o->config_file, &info);
    if (r != 0)
    {
        log_err("qfa query volume[%s]  failed!", o->volume_name);
        r = -1;
        goto cleanup;
    }
    f->real_file_size = info.size;
    return 0;

cleanup:
    fio_qfa_cleanup(td);
    return r;
}

static int fio_qfa_open(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int fio_qfa_invalidate(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static void fio_qfa_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_qfa_iou *fri = io_u->engine_data;

	if (fri) {
		io_u->engine_data = NULL;
		free(fri);
	}
}

static int fio_qfa_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct fio_qfa_iou *fri;

	fri = calloc(1, sizeof(*fri));
	fri->io_u = io_u;
	io_u->engine_data = fri;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name			= "qfa",
	.version		= FIO_IOOPS_VERSION,
	.setup			= fio_qfa_setup,
	.init			= fio_qfa_init,
	.queue			= fio_qfa_queue,
	.getevents		= fio_qfa_getevents,
	.event			= fio_qfa_event,
	.cleanup		= fio_qfa_cleanup,
	.open_file		= fio_qfa_open,
	.invalidate		= fio_qfa_invalidate,
	.options		= options,
	.io_u_init		= fio_qfa_io_u_init,
	.io_u_free		= fio_qfa_io_u_free,
	.option_struct_size	= sizeof(struct qfa_options),
};

static void fio_init fio_qfa_register(void)
{
      register_ioengine(&ioengine);
}

static void fio_exit fio_qfa_unregister(void)
{
      unregister_ioengine(&ioengine);
}
