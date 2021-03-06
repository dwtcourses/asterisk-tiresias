/*
 * application_handler.c
 *
 *  Created on: Jun 16, 2018
 *      Author: pchero
 */


#include <asterisk.h>
#include <asterisk/logger.h>
#include <asterisk/app.h>
#include <asterisk/strings.h>
#include <asterisk/module.h>
#include <asterisk/file.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>


#include <stdio.h>
#include <stdbool.h>
#include <jansson.h>

#include "app_tiresias.h"
#include "fp_handler.h"
#include "application_handler.h"


/*** DOCUMENTATION
	<application name="Tiresias" language="en_US">
		<synopsis>
			Do the tiresias audio fingerprinting
		</synopsis>
		<syntax>
			<parameter name="context" required="true">
				<para>context name.</para>
			</parameter>
			<parameter name="duration">
				<para>fingerprint duration</para>
			</parameter>
			<parameter name="tolerance">
				<para>tolreance score</para>
			</parameter>
			<parameter name="freq_ignore_low">
				<para>Ignore frequency low</para>
			</parameter>
			<parameter name="freq_ignore_high">
				<para>Ignore frequency high</para>
			</parameter>
		</syntax>
		<description>
			<para>Fingerprint and audio recognise with the given seconds.</para>
		</description>
	</application>
 ***/

#define sfree(p) { if(p != NULL) ast_free(p); p=NULL; }

#define DEF_APPLICATION_TIRESIAS "Tiresias"

#define DEF_DURATION   3000


static int tiresias_exec(struct ast_channel *chan, const char *data);
static int record_voice(struct ast_filestream* file, struct ast_channel *chan, int duration);

static int tiresias_exec(struct ast_channel *chan, const char *data)
{
	int ret;
	char* data_copy;
	char* filename;
	char* tmp;
	const char* tmp_const;
	const char* context;
	int duration;
	struct ast_filestream* file;
	double tolerance;
	struct ast_json* j_fp;
	int freq_ignore_low;
	int freq_ignore_high;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(context);
		AST_APP_ARG(duraion);
		AST_APP_ARG(tolerance);
		AST_APP_ARG(freq_ignore_low);
		AST_APP_ARG(freq_ignore_high);
	);

	if (ast_strlen_zero(data) == 1) {
		ast_log(LOG_WARNING, "TIRESIAS requires an argument.\n");
		return -1;
	}
	ast_log(LOG_DEBUG, "Check value. data[%s]\n", data);

	/* parse the args */
	data_copy = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, data_copy);

	/* get context */
	ret = ast_strlen_zero(args.context);
	if(ret == 1) {
		ast_log(LOG_NOTICE, "Wrong context info.\n");
		return -1;
	}
	context = args.context;

	/* get duration */
	duration = DEF_DURATION;
	ret = ast_strlen_zero(args.duraion);
	if(ret != 1) {
		duration = atoi(args.duraion);
	}

	/* get tolerance */
	tolerance = -1;
	tmp_const = ast_json_string_get(ast_json_object_get(ast_json_object_get(g_app->j_conf, "global"), "tolerance"));
	if(tmp_const != NULL) {
		tolerance = atof(tmp_const);
	}
	ret = ast_strlen_zero(args.tolerance);
	if(ret != 1) {
		tolerance = atof(args.tolerance);
	}

	/* get freq_ignore_low */
	freq_ignore_low = -1;
	ret = ast_strlen_zero(args.freq_ignore_low);
	if(ret != 1) {
		freq_ignore_low = atoi(args.freq_ignore_low);
	}

	/* get freq_ignore_high */
	freq_ignore_high = -1;
	ret = ast_strlen_zero(args.freq_ignore_high);
	if(ret != 1) {
		freq_ignore_high = atoi(args.freq_ignore_high);
	}

	/* check values */
	ast_log(LOG_VERBOSE, "Application tiresias. context[%s], durtion[%d], tolerance[%f], freq_ignore_low[%d], freq_ignore_high[%d]\n",
			context, duration, tolerance, freq_ignore_low, freq_ignore_high
			);

	ret = ast_channel_state(chan);
	ast_log(LOG_VERBOSE, "Channel state. state[%d]\n", ret);
	if(ret != AST_STATE_UP) {
		/* answer */
		ret = ast_answer(chan);
	}

	/* create temp file */
	tmp = fp_generate_uuid();
	ast_asprintf(&filename, "/tmp/tiresias-%s", tmp);
	sfree(tmp);
	file = 	ast_writefile(filename, "wav", NULL, O_CREAT|O_TRUNC|O_WRONLY, 0, AST_FILE_MODE);
	if(file == NULL) {
		ast_log(LOG_NOTICE, "Could not create temp recording file.\n");
		sfree(filename);
		return -1;
	}

	/* record */
	ret = record_voice(file, chan, duration);
	ast_closestream(file);
	if(ret == 0) {
		pbx_builtin_setvar_helper(chan, "TIRSTATUS", "HANGUP");
		ast_filedelete(filename, NULL);
		sfree(filename);
		return 0;
	}
	else if(ret < 0) {
		pbx_builtin_setvar_helper(chan, "TIRSTATUS", "NOTFOUND");
		ast_filedelete(filename, NULL);
		sfree(filename);
		return 0;
	}

	/* do the fingerprinting and recognition */
	ast_asprintf(&tmp, "%s.wav", filename);
	j_fp = fp_search_fingerprint_info(context, tmp, 1, tolerance, freq_ignore_low, freq_ignore_high);

	/* delete file */
	ast_filedelete(filename, NULL);
	sfree(filename);
	sfree(tmp);

	if(j_fp == NULL) {
		ast_log(LOG_VERBOSE, "Could not get fingerprint info.");
		pbx_builtin_setvar_helper(chan, "TIRSTATUS", "NOTFOUND");
		return 0;
	}

	/* TIRSTATUS */
	pbx_builtin_setvar_helper(chan, "TIRSTATUS", "FOUND");

	/* TIRFRAMECOUNT */
	ret = ast_json_integer_get(ast_json_object_get(j_fp, "frame_count"));
	ast_asprintf(&tmp, "%d", ret);
	pbx_builtin_setvar_helper(chan, "TIRFRAMECOUNT", tmp);
	sfree(tmp);

	/* TIRMATCHCOUNT */
	ret = ast_json_integer_get(ast_json_object_get(j_fp, "match_count"));
	ast_asprintf(&tmp, "%d", ret);
	pbx_builtin_setvar_helper(chan, "TIRMATCHCOUNT", tmp);
	sfree(tmp);

	/* TIRFILEUUID */
	tmp_const = ast_json_string_get(ast_json_object_get(j_fp, "uuid"));
	if(tmp_const == NULL) {
		ast_log(LOG_ERROR, "Could not get uuid info.\n");
	}
	pbx_builtin_setvar_helper(chan, "TIRFILEUUID", tmp_const? : "");

	/* TIRFILENAME */
	tmp_const = ast_json_string_get(ast_json_object_get(j_fp, "name"));
	if(tmp_const == NULL) {
		ast_log(LOG_ERROR, "Could not get name info.\n");
	}
	pbx_builtin_setvar_helper(chan, "TIRFILENAME", tmp_const? : "");

	/* TIRCONTEXT */
	tmp_const = ast_json_string_get(ast_json_object_get(j_fp, "context"));
	if(tmp_const == NULL) {
		ast_log(LOG_ERROR, "Could not get context info.\n");
	}
	pbx_builtin_setvar_helper(chan, "TIRCONTEXT", tmp_const? : "");

	/* TIRFILEHASH */
	tmp_const = ast_json_string_get(ast_json_object_get(j_fp, "hash"));
	if(tmp_const == NULL) {
		ast_log(LOG_ERROR, "Could not get hash info.\n");
	}
	pbx_builtin_setvar_helper(chan, "TIRFILEHASH", tmp_const? : "");

	ast_json_unref(j_fp);

	return 0;
}

/**
 * Record the given channel.
 * @param file
 * @param chan
 * @param duration
 * @return 1:success, 0:hangup, -1:error
 */
static int record_voice(struct ast_filestream* file, struct ast_channel *chan, int duration)
{
	int ret;
	struct timeval start;
	struct ast_frame* frame;
	int ms;
	int err;

	if((file == NULL) || (chan == NULL) || (duration < 0)) {
		ast_log(LOG_WARNING, "Wrong input parameter.\n");
		return -1;
	}

	err = 0;
	start = ast_tvnow();
	frame = NULL;
	while(1) {

		ms = ast_remaining_ms(start, duration);
		if(ms <= 0) {
			break;
		}

		ms = ast_waitfor(chan, ms);
		if(ms < 0) {
			break;
		}

		if((duration > 0) && (ms == 0)) {
			break;
		}

		/* read the frame */
		frame = ast_read(chan);
		if(frame == NULL) {
			/* hangup */
			ast_log(LOG_VERBOSE, "The channel has been hungup.\n");
			err = 1;
			break;
		}

		if(frame->frametype != AST_FRAME_VOICE) {
			ast_log(LOG_DEBUG, "Check frame type. frame_type[%d]\n", frame->frametype);
			ast_frfree(frame);
			continue;
		}

		ret = ast_writestream(file, frame);
		ast_frfree(frame);
		if(ret != 0) {
			ast_log(LOG_WARNING, "Problem writing frame.\n");
			err = 2;
			break;
		}
	}

	if(err == 1) {
		return 0;
	}
	else if(err > 1) {
		return -1;
	}

	return 1;
}

bool application_init(void)
{
	int ret;

	ast_log(LOG_VERBOSE, "init_application_handler.\n");

	ret = ast_register_application2(DEF_APPLICATION_TIRESIAS, tiresias_exec, NULL, NULL, NULL);

	if(ret != 0) {
		return false;
	}

	return true;
}

bool application_term(void)
{
	ast_log(LOG_VERBOSE, "term_application_handler.\n");

	ast_unregister_application(DEF_APPLICATION_TIRESIAS);

	return true;
}
