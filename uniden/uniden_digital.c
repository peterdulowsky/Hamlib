/*
 *  Hamlib Uniden backend - uniden_digital backend
 *  Copyright (c) 2001-2008 by Stephane Fillod
 *
 *	$Id: uniden_digital.c,v 1.3 2008-10-18 06:21:31 roger-linux Exp $
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <math.h>

#include "hamlib/rig.h"
#include "serial.h"
#include "misc.h"

#include "uniden_digital.h"


/*
 * Uniden Digital backend should work for:
 * BCD996T as well as the BCD396T.  Some protocols available for the
 * BCD996T may not be available for the BCD396T or modified to work.
 *
 * Protocol information can be found here:
 * http://www.uniden.com/files/BCD396T_Protocol.pdf
 * http://www.uniden.com/files/BCD996T_Protocol.pdf
 *
 * There are undocumented commands such as firmware_dump and
 * firmware_load.  These commands are defined within DSctl code.
 *
 * There are two methods of retrieving the next memory location
 * (aka frequency bank).  Use either the "Get Next Location" or 
 * use the address returned from one of the commands.  If you decide
 * the latter method, the order is slightly confusing but, I have it
 * well documented within DSctl.  The latter method is also as much
 * as 30% faster then using the Uniden software or "Get Next
 * Location" command.
 */

static const struct { rig_model_t model; const char *id; }
uniden_id_string_list[] = {
	{ RIG_MODEL_BCD396T,  "BCD396T" },
	{ RIG_MODEL_BCD996T,  "BCD99tT" },
	{ RIG_MODEL_NONE, NULL },	/* end marker */
};


#define EOM "\r"

#define BUFSZ 64

/**
 * uniden_transaction
 * uniden_digital_transaction
 * Assumes rig!=NULL rig->state!=NULL rig->caps!=NULL
 *
 * cmdstr - Command to be sent to the rig. Cmdstr can also be NULL, indicating
 *          that only a reply is needed (nothing will be send).
 * replystr - Reply prefix to be expected. Replystr can also be NULL, indicating
 *          that the prefix is either the cmdstr prefix or OK.
 * data - Buffer for reply string.  Can be NULL, indicating that no reply is
 *        is needed and will return with RIG_OK after command was sent.
 * datasize - in: Size of buffer. It is the caller's responsibily to provide
 *            a large enough buffer for all possible replies for a command.
 *            out: location where to store number of bytes read.
 *
 * returns:
 *   RIG_OK  -  if no error occured.
 *   RIG_EIO  -  if an I/O error occured while sending/receiving data.
 *   RIG_ETIMEOUT  -  if timeout expires without any characters received.
 *   RIG_REJECTED  -  if a negative acknowledge was received or command not
 *                    recognized by rig.
 */
int
uniden_digital_transaction (RIG *rig, const char *cmdstr, int cmd_len, const char *replystr,
				char *data, size_t *datasize)
{
    struct rig_state *rs;
    int retval;
    int retry_read = 0;
    char replybuf[BUFSZ];
    size_t reply_len=BUFSZ;

    rs = &rig->state;
    rs->hold_decode = 1;

transaction_write:

    serial_flush(&rs->rigport);

    if (cmdstr) {
        retval = write_block(&rs->rigport, cmdstr, strlen(cmdstr));
        if (retval != RIG_OK)
            goto transaction_quit;
    }

    /* Always read the reply to known if it went OK */
    if (!data)
	    data = replybuf;
    if (!datasize)
	    datasize = &reply_len;

    memset(data,0,*datasize);
    retval = read_string(&rs->rigport, data, *datasize, EOM, strlen(EOM));
    if (retval < 0) {
        if (retry_read++ < rig->state.rigport.retry)
            goto transaction_write;
        goto transaction_quit;
    }	else
  	    *datasize = retval;

    /* Check that command termination is correct */
    if (strchr(EOM, data[strlen(data)-1])==NULL) {
        rig_debug(RIG_DEBUG_ERR, "%s: Command is not correctly terminated '%s'\n", __FUNCTION__, data);
        if (retry_read++ < rig->state.rigport.retry)
            goto transaction_write;
        retval = -RIG_EPROTO;
        goto transaction_quit;
    }

    if (strcmp(data, "OK"EOM)) {
	    /* everything is fine */
	    retval = RIG_OK;
	    goto transaction_quit;
    }

    /*  Any syntax returning NG indicates a VALID Command but not entered
     *  in the right mode or using the correct parameters. ERR indicates
     *  an INVALID Command.
     */
    if (strcmp(data, "NG"EOM) || strcmp(data, "ORER"EOM)) {
	    /* Invalid command */
	    rig_debug(RIG_DEBUG_VERBOSE, "%s: NG/Overflow for '%s'\n", __FUNCTION__, cmdstr);
	    retval = -RIG_EPROTO;
	    goto transaction_quit;
    }

    if (strcmp(data, "ERR"EOM)) {
	    /*  Command format error */
	    rig_debug(RIG_DEBUG_VERBOSE, "%s: Error for '%s'\n", __FUNCTION__, cmdstr);
	    retval = -RIG_EINVAL;
	    goto transaction_quit;
    }

#define CONFIG_STRIP_CMDTRM 1
#ifdef CONFIG_STRIP_CMDTRM
    if (strlen(data) > 0)
    	data[strlen(data)-1] = '\0';  /* not very elegant, but should work. */
    else
    	data[0] = '\0';
#endif

    /* Special case for SQuelch */
    if (!memcmp(cmdstr,"SQ",2) && (replystr[0] == '-' || replystr[0] == '+')) {
	    retval = RIG_OK;
	    goto transaction_quit;
	}

    /* Command prefix if no replystr supplied */
    if (!replystr)
	    replystr = cmdstr;
    /*
     * Check that received the correct reply. The first two characters
     * should be the same as command.
     */
    if (replystr && replystr[0] && (data[0] != replystr[0] || 
			    (replystr[1] && data[1] != replystr[1]))) {
         /*
          * TODO: When RIG_TRN is enabled, we can pass the string
          *       to the decoder for callback. That way we don't ignore
          *       any commands.
          */
        rig_debug(RIG_DEBUG_ERR, "%s: Unexpected reply '%s'\n", __FUNCTION__, data);
        if (retry_read++ < rig->state.rigport.retry)
            goto transaction_write;

        retval =  -RIG_EPROTO;
        goto transaction_quit;
    }

    retval = RIG_OK;
transaction_quit:
    rs->hold_decode = 0;
    return retval;
}


/*
 * uniden_get_info
 * Assumes rig!=NULL
 */
const char * uniden_digital_get_info(RIG *rig)
{
	static char infobuf[BUFSZ];
	size_t info_len=BUFSZ/2, vrinfo_len=BUFSZ/2;
	int ret;

	ret = uniden_digital_transaction (rig, "STS" EOM, 3, NULL, infobuf, &info_len);
	if (ret != RIG_OK)
		return NULL;

	/* SI BC250D,0000000000,104  */

	if (info_len < 4)
		return NULL;

	if (info_len >= BUFSZ)
		info_len = BUFSZ-1;
	infobuf[info_len] = '\0';

	/* VR not on every rig */
	/* VR1.00 */
	ret = uniden_digital_transaction (rig, "MDL" EOM, 3, NULL, infobuf+info_len, &vrinfo_len);
	if (ret == RIG_OK)
	{
		/* overwrite "VR" */
		/* FIXME: need to filter \r or it screws w/ stdout */
		infobuf[info_len] = '\n';
		infobuf[info_len+1] = ' ';
	}
	else
	{
		infobuf[info_len] = '\0';
	}

	/* skip "SI " */
	return infobuf+3;
}


/*
 * skeleton
 */
int uniden_digital_set_freq(RIG *rig, vfo_t vfo, freq_t freq)
{
#if 0
	char freqbuf[BUFSZ];
	size_t freq_len=BUFSZ;

	/* freq in hundreds of Hz */
	freq /= 100;

	/* exactly 8 digits */
	freq_len = sprintf(freqbuf, "RF%08u" EOM, (unsigned)freq);

	return uniden_transaction (rig, freqbuf, freq_len, NULL, NULL, NULL);
#else
	return -RIG_ENIMPL;
#endif
}

/*
 * skeleton
 */
int uniden_digital_get_freq(RIG *rig, vfo_t vfo, freq_t *freq)
{
#if 0
	char freqbuf[BUFSZ];
	size_t freq_len=BUFSZ;
	int ret;

	ret = uniden_transaction (rig, "RF" EOM, 3, NULL, freqbuf, &freq_len);
	if (ret != RIG_OK)
		return ret;

	if (freq_len < 10)
		return -RIG_EPROTO;

	sscanf(freqbuf+2, "%"SCNfreq, freq);
	/* returned freq in hundreds of Hz */
	*freq *= 100;

	return RIG_OK;
#else
	return -RIG_ENIMPL;
#endif
}

