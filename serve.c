#include "cache.h"
#include "repository.h"
#include "config.h"
#include "pkt-line.h"
#include "version.h"
#include "strvec.h"
#include "ls-refs.h"
#include "protocol-caps.h"
#include "serve.h"
#include "upload-pack.h"

static int advertise_sid;

static int always_advertise(struct repository *r,
			    struct strbuf *value)
{
	return 1;
}

static int agent_advertise(struct repository *r,
			   struct strbuf *value)
{
	if (value)
		strbuf_addstr(value, git_user_agent_sanitized());
	return 1;
}

static int object_format_advertise(struct repository *r,
				   struct strbuf *value)
{
	if (value)
		strbuf_addstr(value, r->hash_algo->name);
	return 1;
}

static int session_id_startup_config(const char *var, const char *value, void *data)
{
	if (!strcmp(var, "transfer.advertisesid"))
		advertise_sid = git_config_bool(var, value);
	return 0;
}

static int session_id_advertise(struct repository *r, struct strbuf *value)
{
	if (!advertise_sid)
		return 0;
	if (value)
		strbuf_addstr(value, trace2_session_id());
	return 1;
}

typedef int (*advertise_fn_t)(struct repository *r, struct strbuf *value);
typedef int (*command_fn_t)(struct repository *r, struct strvec *keys,
			    struct packet_reader *request);

struct protocol_capability {
	/*
	 * The name of the capability.  The server uses this name when
	 * advertising this capability, and the client uses this name to
	 * specify this capability.
	 */
	const char *name;

	/*
	 * A git_config() callback that'll be called only once for the
	 * lifetime of the process, possibly over many different
	 * requests. Used for reading config that's expected to be
	 * static.
	 *
	 * The "command" or "advertise" callbacks themselves are
	 * expected to read config that needs to be more current than
	 * that, or which is dependent on request data.
	 */
	int (*startup_config)(const char *var, const char *value, void *data);

	/*
	 * A boolean to check if we've called our "startup_config"
	 * callback.
	 */
	int have_startup_config;

	/*
	 * Function queried to see if a capability should be advertised.
	 * Optionally a value can be specified by adding it to 'value'.
	 * If a value is added to 'value', the server will advertise this
	 * capability as "<name>=<value>" instead of "<name>".
	 */
	advertise_fn_t advertise;

	/*
	 * Function called when a client requests the capability as a command.
	 * The function will be provided the capabilities requested via 'keys'
	 * as well as a struct packet_reader 'request' which the command should
	 * use to read the command specific part of the request.  Every command
	 * MUST read until a flush packet is seen before sending a response.
	 *
	 * This field should be NULL for capabilities which are not commands.
	 */
	command_fn_t command;
};

static struct protocol_capability capabilities[] = {
	{
		.name = "agent",
		.advertise = agent_advertise,
	},
	{
		.name = "ls-refs",
		.startup_config = ls_refs_startup_config,
		.advertise = ls_refs_advertise,
		.command = ls_refs,
	},
	{
		.name = "fetch",
		.startup_config = serve_upload_pack_startup_config,
		.advertise = upload_pack_advertise,
		.command = upload_pack_v2,
	},
	{
		.name = "server-option",
		.advertise = always_advertise,
	},
	{
		.name = "object-format",
		.advertise = object_format_advertise,
	},
	{
		.name = "session-id",
		.startup_config = session_id_startup_config,
		.advertise = session_id_advertise,
	},
	{
		.name = "object-info",
		.advertise = always_advertise,
		.command = cap_object_info,
	},
};

static void read_startup_config(struct protocol_capability *command)
{
	if (!command->startup_config)
		return;
	if (command->have_startup_config++)
		return;
	git_config(command->startup_config, NULL);
}

static int call_advertise(struct protocol_capability *command,
			  struct repository *r, struct strbuf *value)
{
	int ret;
	struct strbuf sb = STRBUF_INIT;
	const char *msg;

	read_startup_config(command);

	strbuf_addf(&sb, "advertise/%s", command->name);
	trace2_region_enter("serve", sb.buf, r);
	ret = command->advertise(r, value);
	msg = ret ? "advertised" : "hidden";
	trace2_region_leave_printf("serve", sb.buf, r, "%s", msg);
	strbuf_release(&sb);

	return ret;
}

static int call_command(struct protocol_capability *command,
			struct repository *r, struct strvec *keys,
			struct packet_reader *request)
{
	int ret;
	struct strbuf sb = STRBUF_INIT;

	read_startup_config(command);

	strbuf_addf(&sb, "command/%s", command->name);
	trace2_region_enter("serve", sb.buf, r);
	ret = command->command(r, keys, request);
	trace2_region_leave("serve", sb.buf, r);
	strbuf_release(&sb);

	return ret;
}

static void advertise_capabilities(void)
{
	struct strbuf capability = STRBUF_INIT;
	struct strbuf value = STRBUF_INIT;
	int i;

	for (i = 0; i < ARRAY_SIZE(capabilities); i++) {
		struct protocol_capability *c = &capabilities[i];

		if (call_advertise(c, the_repository, &value)) {
			strbuf_addstr(&capability, c->name);

			if (value.len) {
				strbuf_addch(&capability, '=');
				strbuf_addbuf(&capability, &value);
			}

			strbuf_addch(&capability, '\n');
			packet_write(1, capability.buf, capability.len);
		}

		strbuf_reset(&capability);
		strbuf_reset(&value);
	}

	packet_flush(1);
	strbuf_release(&capability);
	strbuf_release(&value);
}

static struct protocol_capability *get_capability(const char *key)
{
	int i;

	if (!key)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(capabilities); i++) {
		struct protocol_capability *c = &capabilities[i];
		const char *out;
		if (skip_prefix(key, c->name, &out) && (!*out || *out == '='))
			return c;
	}

	return NULL;
}

static int is_valid_capability(const char *key)
{
	struct protocol_capability *c = get_capability(key);

	return c && call_advertise(c, the_repository, NULL);
}

static int is_command(const char *key, struct protocol_capability **command)
{
	const char *out;

	if (skip_prefix(key, "command=", &out)) {
		struct protocol_capability *cmd = get_capability(out);

		if (*command)
			die("command '%s' requested after already requesting command '%s'",
			    out, (*command)->name);
		if (!cmd || !call_advertise(cmd, the_repository, NULL) || !cmd->command)
			die("invalid command '%s'", out);

		*command = cmd;
		return 1;
	}

	return 0;
}

static int has_capability(const struct strvec *keys, const char *capability,
			  const char **value)
{
	int i;
	for (i = 0; i < keys->nr; i++) {
		const char *out;
		if (skip_prefix(keys->v[i], capability, &out) &&
		    (!*out || *out == '=')) {
			if (value) {
				if (*out == '=')
					out++;
				*value = out;
			}
			return 1;
		}
	}

	return 0;
}

static void check_algorithm(struct repository *r, struct strvec *keys)
{
	int client = GIT_HASH_SHA1, server = hash_algo_by_ptr(r->hash_algo);
	const char *algo_name;

	if (has_capability(keys, "object-format", &algo_name)) {
		client = hash_algo_by_name(algo_name);
		if (client == GIT_HASH_UNKNOWN)
			die("unknown object format '%s'", algo_name);
	}

	if (client != server)
		die("mismatched object format: server %s; client %s\n",
		    r->hash_algo->name, hash_algos[client].name);
}

enum request_state {
	PROCESS_REQUEST_KEYS,
	PROCESS_REQUEST_DONE,
};

static int process_request(void)
{
	enum request_state state = PROCESS_REQUEST_KEYS;
	struct packet_reader reader;
	struct strvec keys = STRVEC_INIT;
	struct protocol_capability *command = NULL;
	const char *client_sid;

	packet_reader_init(&reader, 0, NULL, 0,
			   PACKET_READ_CHOMP_NEWLINE |
			   PACKET_READ_GENTLE_ON_EOF |
			   PACKET_READ_DIE_ON_ERR_PACKET);

	/*
	 * Check to see if the client closed their end before sending another
	 * request.  If so we can terminate the connection.
	 */
	if (packet_reader_peek(&reader) == PACKET_READ_EOF)
		return 1;
	reader.options &= ~PACKET_READ_GENTLE_ON_EOF;

	while (state != PROCESS_REQUEST_DONE) {
		switch (packet_reader_peek(&reader)) {
		case PACKET_READ_EOF:
			BUG("Should have already died when seeing EOF");
		case PACKET_READ_NORMAL:
			/* collect request; a sequence of keys and values */
			if (is_command(reader.line, &command) ||
			    is_valid_capability(reader.line))
				strvec_push(&keys, reader.line);
			else
				die("unknown capability '%s'", reader.line);

			/* Consume the peeked line */
			packet_reader_read(&reader);
			break;
		case PACKET_READ_FLUSH:
			/*
			 * If no command and no keys were given then the client
			 * wanted to terminate the connection.
			 */
			if (!keys.nr)
				return 1;

			/*
			 * The flush packet isn't consume here like it is in
			 * the other parts of this switch statement.  This is
			 * so that the command can read the flush packet and
			 * see the end of the request in the same way it would
			 * if command specific arguments were provided after a
			 * delim packet.
			 */
			state = PROCESS_REQUEST_DONE;
			break;
		case PACKET_READ_DELIM:
			/* Consume the peeked line */
			packet_reader_read(&reader);

			state = PROCESS_REQUEST_DONE;
			break;
		case PACKET_READ_RESPONSE_END:
			BUG("unexpected stateless separator packet");
		}
	}

	if (!command)
		die("no command requested");

	check_algorithm(the_repository, &keys);

	if (has_capability(&keys, "session-id", &client_sid))
		trace2_data_string("transfer", NULL, "client-sid", client_sid);

	call_command(command, the_repository, &keys, &reader);

	strvec_clear(&keys);
	return 0;
}

/* Main serve loop for protocol version 2 */
void serve(struct serve_options *options)
{
	if (options->advertise_capabilities || !options->stateless_rpc) {
		/* serve by default supports v2 */
		packet_write_fmt(1, "version 2\n");

		advertise_capabilities();
		/*
		 * If only the list of capabilities was requested exit
		 * immediately after advertising capabilities
		 */
		if (options->advertise_capabilities)
			return;
	}

	/*
	 * If stateless-rpc was requested then exit after
	 * a single request/response exchange
	 */
	if (options->stateless_rpc) {
		process_request();
	} else {
		for (;;)
			if (process_request())
				break;
	}
}
