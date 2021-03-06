#include "sensorflare.h"

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void die_on_error(int x, char const *context) {
    if (x < 0) {
	fprintf(stderr, "%s: %s\n", context, amqp_error_string2(x));
	exit(1);
    }
}

void die_on_amqp_error(amqp_rpc_reply_t x, char const *context) {
    switch (x.reply_type) {
	case AMQP_RESPONSE_NORMAL:
	    return;

	case AMQP_RESPONSE_NONE:
	    fprintf(stderr, "%s: missing RPC reply type!\n", context);
	    break;

	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
	    fprintf(stderr, "%s: %s\n", context, amqp_error_string2(x.library_error));
	    break;

	case AMQP_RESPONSE_SERVER_EXCEPTION:
	    switch (x.reply.id) {
		case AMQP_CONNECTION_CLOSE_METHOD:
		{
		    amqp_connection_close_t *m = (amqp_connection_close_t *) x.reply.decoded;
		    fprintf(stderr, "%s: server connection error %d, message: %.*s\n",
			    context,
			    m->reply_code,
			    (int) m->reply_text.len, (char *) m->reply_text.bytes);
		    break;
		}
		case AMQP_CHANNEL_CLOSE_METHOD:
		{
		    amqp_channel_close_t *m = (amqp_channel_close_t *) x.reply.decoded;
		    fprintf(stderr, "%s: server channel error %d, message: %.*s\n",
			    context,
			    m->reply_code,
			    (int) m->reply_text.len, (char *) m->reply_text.bytes);
		    break;
		}
		default:
		    fprintf(stderr, "%s: unknown server error, method id 0x%08X\n", context, x.reply.id);
		    break;
	    }
	    break;
    }

    exit(1);
}

static void dump_row(long count, int numinrow, int *chs) {
    int i;

    printf("%08lX:", count - numinrow);

    if (numinrow > 0) {
	for (i = 0; i < numinrow; i++) {
	    if (i == 8) {
		printf(" :");
	    }
	    printf(" %02X", chs[i]);
	}
	for (i = numinrow; i < 16; i++) {
	    if (i == 8) {
		printf(" :");
	    }
	    printf("   ");
	}
	printf("  ");
	for (i = 0; i < numinrow; i++) {
	    if (isprint(chs[i])) {
		printf("%c", chs[i]);
	    } else {
		printf(".");
	    }
	}
    }
    printf("\n");
}

static int rows_eq(int *a, int *b) {
    int i;

    for (i = 0; i < 16; i++)
	if (a[i] != b[i]) {
	    return 0;
	}

    return 1;
}

void amqp_dump(void const *buffer, size_t len) {
    unsigned char *buf = (unsigned char *) buffer;
    long count = 0;
    int numinrow = 0;
    int chs[16];
    int oldchs[16] = {0};
    int showed_dots = 0;
    size_t i;

    for (i = 0; i < len; i++) {
	int ch = buf[i];

	if (numinrow == 16) {
	    int i;

	    if (rows_eq(oldchs, chs)) {
		if (!showed_dots) {
		    showed_dots = 1;
		    printf("          .. .. .. .. .. .. .. .. : .. .. .. .. .. .. .. ..\n");
		}
	    } else {
		showed_dots = 0;
		dump_row(count, numinrow, chs);
	    }

	    for (i = 0; i < 16; i++) {
		oldchs[i] = chs[i];
	    }

	    numinrow = 0;
	}

	count++;
	chs[numinrow++] = ch;
    }

    dump_row(count, numinrow, chs);

    if (numinrow != 0) {
	printf("%08lX:\n", count);
    }
}

void sendMessage(char * messagebody) {

    amqp_connection_state_t conn = amqp_new_connection();

    amqp_socket_t * rabbit_socket = amqp_tcp_socket_new(conn);
    if (!rabbit_socket) {
	die("creating TCP/SSL/TLS socket");
    }

    /*
	status = amqp_ssl_socket_set_cacert(rabbit_socket, "ca.pem");
	if (status) {
	  die("setting CA certificate");
	}
     */

    int status = amqp_socket_open(rabbit_socket, "mochad.sensorflare.com", 5672);
    if (status) {
	die("opening TCP/SSL/TLS connection");
    }


    die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, username, password), "Logging in");
    amqp_channel_open(conn, 1);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");


    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes("text/plain");
    props.delivery_mode = 1; /* persistent delivery mode */
    die_on_error(amqp_basic_publish(conn,
	    1,
	    amqp_cstring_bytes(exchange),
	    amqp_cstring_bytes(exchange),
	    0,
	    0,
	    &props,
	    amqp_cstring_bytes(messagebody)),
	    "Publishing");
    syslog(LOG_NOTICE, "Sent message : %s , exchange: %s", messagebody, exchange);
    syslog(LOG_NOTICE, "Receiver Status: %d", receiver_thread_status);

    die_on_amqp_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS), "Closing channel");
    die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS), "Closing connection");
    die_on_error(amqp_destroy_connection(conn), "Ending connection");
}

void * status_reporting(void *threadid) {
    char st_command[10];
    sprintf(st_command, "st");
    while (1) {
	processcommandline(NULL, st_command);
	sleep(STATUS_INTERVAL);
    }
    return threadid;
}

void * receiver(void *receiver_thread_status_p) {
    (*(int *) receiver_thread_status_p) = 1;
    while (1) {

	amqp_rpc_reply_t res;
	amqp_envelope_t envelope;

	amqp_maybe_release_buffers(conn);


	res = amqp_consume_message(conn, &envelope, NULL, 0);
	if (AMQP_RESPONSE_NORMAL != res.reply_type) {
	    syslog(LOG_NOTICE, "not an AMQP_RESPONSE_NORMAL %d", res.reply_type);
	    break;
	}


	syslog(LOG_NOTICE, "Delivery %u, exchange '%.*s' routingkey '%.*s'\n",
		(unsigned) envelope.delivery_tag,
		(int) envelope.exchange.len, (char *) envelope.exchange.bytes,
		(int) envelope.routing_key.len, (char *) envelope.routing_key.bytes);

	char buf[100];
	syslog(LOG_INFO, "%s", (char *) envelope.message.body.bytes);

	memcpy(buf, envelope.message.body.bytes, envelope.message.body.len);
	sprintf(buf, "%s", (char *) envelope.message.body.bytes);
	buf[envelope.message.body.len] = '\0';
	printf("%s", buf);
	processcommandline(NULL, buf);


	if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
	    /*
			syslog("Content-type: %.*s\n",
				(int) envelope.message.properties.content_type.len,
				(char *) envelope.message.properties.content_type.bytes);
	     */
	}


	syslog(LOG_NOTICE, "Receive command from rabbitmq: %s", buf);
	/*
	 */
	/*
		syslog("\n");
	 */
	//amqp_dump(envelope.message.body.bytes, envelope.message.body.len);

	/*
		if (strcmp("end", buf) == 0) {
		    amqp_destroy_envelope(&envelope);
		    break;
		}
	 */
	amqp_destroy_envelope(&envelope);
    }

    (*(int *) receiver_thread_status_p) = 0;

    die_on_amqp_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS), "Closing channel");
    die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS), "Closing connection");
    die_on_error(amqp_destroy_connection(conn), "Ending connection");

    syslog(LOG_NOTICE, "receiver exited!");


    init_sensorflare(0);

    return receiver_thread_status_p;
}

void init_sensorflare(long int Cm19a) {

    FILE* fptr = fopen("/etc/mochad/sensorflare.conf", "r");
    sensorflare_connected = fptr != NULL;
    if (!sensorflare_connected) {
	return;
    }

    cfg_opt_t opts[] = {
	CFG_STR("password", "password", CFGF_NONE),
	CFG_STR("username", "username", CFGF_NONE),
	CFG_END()
    };
    cfg_t *cfg;

    cfg = cfg_init(opts, CFGF_NONE);
    if (cfg_parse(cfg, "/etc/mochad/sensorflare.conf") == CFG_PARSE_ERROR) {
	syslog(LOG_ERR, "failed to parse /etc/mochad/sensorflare.conf");
	return;
    }

    username = cfg_getstr(cfg, "username");
    password = cfg_getstr(cfg, "password");

    sprintf(exchange, "mochad-%s-send", username);
    sprintf(commands_queue, "mochad-%s-commands", username);
    syslog(LOG_INFO, "send exchange : %s", exchange);
    syslog(LOG_INFO, "commands queue : %s", commands_queue);

    syslog(LOG_INFO, "Connecting to rabbitmq endpoint mochad.sensorflare.com:5671...");

    int status;

    conn = amqp_new_connection();

    rabbit_socket = amqp_tcp_socket_new(conn);
    if (!rabbit_socket) {
	die("creating TCP/SSL/TLS socket");
    }

    /*
	status = amqp_ssl_socket_set_cacert(rabbit_socket, "ca.pem");
	if (status) {
	  die("setting CA certificate");
	}
     */

    status = amqp_socket_open(rabbit_socket, "mochad.sensorflare.com", 5672);
    if (status) {
	die("opening TCP/SSL/TLS connection");
    }


    die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, username, password), "Logging in");
    amqp_channel_open(conn, 1);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");

    amqp_basic_consume(conn, 1, amqp_cstring_bytes(commands_queue), amqp_empty_bytes, 0, 0, 0, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");

    int rc = pthread_create(&rabbit_receiver_thread, NULL, receiver, (void *) &receiver_thread_status);
    if (rc) {
	syslog(LOG_ERR, "return code from pthread_create() is %d\n", rc);
	exit(-1);
    }

}