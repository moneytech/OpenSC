/*
 * sc-card.c: General SmartCard functions
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sc-internal.h"
#include "sc-log.h"
#include "sc-asn1.h"
#include <assert.h>
#include <stdlib.h>

int sc_sw_to_errorcode(struct sc_card *card, int sw1, int sw2)
{
	if (sw1 != 0x90 || sw2 != 0)
		error(card->ctx, "card returned SW1=%02X, SW2=%02X\n", sw1, sw2);
	switch (sw1) {
	case 0x67:
		if (sw2 == 0)
			return SC_ERROR_WRONG_LENGTH;
		break;
	case 0x69:
		switch (sw2) {
		case 0x82:
			return SC_ERROR_SECURITY_STATUS_NOT_SATISFIED;
		default:
			break;
		}
		break;
	case 0x6A:
		switch (sw2) {
		case 0x81:
			return SC_ERROR_NOT_SUPPORTED;
		case 0x82:
			return SC_ERROR_FILE_NOT_FOUND;
		case 0x83:
			return SC_ERROR_RECORD_NOT_FOUND;
		case 0x85:
		case 0x86:
		case 0x87:
			return SC_ERROR_INVALID_ARGUMENTS;
		default:
			break;
		}
		break;
	case 0x6C:
		error(card->ctx, "incorrect length, right length is %d\n",
		      sw2);
		return SC_ERROR_WRONG_LENGTH;
	case 0x6D:
		return SC_ERROR_NOT_SUPPORTED;
	case 0x6E:
		return SC_ERROR_UNKNOWN_SMARTCARD;
	case 0x90:
		if (sw2 == 0)
			return 0;
	}
	return SC_ERROR_UNKNOWN_REPLY;
}

static int _sc_pcscret_to_error(long rv)
{
	switch (rv) {
	case SCARD_W_REMOVED_CARD:
		return SC_ERROR_CARD_REMOVED;
	case SCARD_W_RESET_CARD:
		return SC_ERROR_CARD_RESET;
	case SCARD_E_NOT_TRANSACTED:
		return SC_ERROR_TRANSMIT_FAILED;
	default:
		return SC_ERROR_UNKNOWN;
	}
}


static int sc_check_apdu(struct sc_context *ctx, const struct sc_apdu *apdu)
{
	switch (apdu->cse) {
	case SC_APDU_CASE_1:
		if (apdu->datalen > 0)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		break;
	case SC_APDU_CASE_2_SHORT:
		if (apdu->datalen > 0)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		if (apdu->resplen < apdu->le)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		break;
	case SC_APDU_CASE_3_SHORT:
		if (apdu->datalen == 0 || apdu->data == NULL)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		break;
	case SC_APDU_CASE_4_SHORT:
		if (apdu->datalen == 0 || apdu->data == NULL)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		if (apdu->resplen < apdu->le)
			SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
		break;
	case SC_APDU_CASE_2_EXT:
	case SC_APDU_CASE_3_EXT:
	case SC_APDU_CASE_4_EXT:
		SC_FUNC_RETURN(ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	}
	return 0;
}

static int sc_transceive_t0(struct sc_card *card, struct sc_apdu *apdu)
{
	SCARD_IO_REQUEST sSendPci, sRecvPci;
	BYTE s[SC_MAX_APDU_BUFFER_SIZE], r[SC_MAX_APDU_BUFFER_SIZE];
	DWORD dwSendLength, dwRecvLength;
	LONG rv;
	u8 *data = s;
	size_t data_bytes = apdu->lc;

	if (data_bytes == 0)
		data_bytes = 256;
	*data++ = apdu->cla;
	*data++ = apdu->ins;
	*data++ = apdu->p1;
	*data++ = apdu->p2;
	switch (apdu->cse) {
	case SC_APDU_CASE_1:
		break;
	case SC_APDU_CASE_2_SHORT:
		*data++ = (u8) apdu->le;
		break;
	case SC_APDU_CASE_2_EXT:
		*data++ = (u8) 0;
		*data++ = (u8) (apdu->le >> 8);
		*data++ = (u8) (apdu->le & 0xFF);
		break;
	case SC_APDU_CASE_3_SHORT:
		*data++ = (u8) apdu->lc;
		if (apdu->datalen != data_bytes)
			return SC_ERROR_INVALID_ARGUMENTS;
		memcpy(data, apdu->data, data_bytes);
		data += data_bytes;
		break;
	case SC_APDU_CASE_4_SHORT:
		*data++ = (u8) apdu->lc;
		if (apdu->datalen != data_bytes)
			return SC_ERROR_INVALID_ARGUMENTS;
		memcpy(data, apdu->data, data_bytes);
		data += data_bytes;
		*data++ = (u8) apdu->le;
		break;
	}

	sSendPci.dwProtocol = SCARD_PROTOCOL_T0;
	sSendPci.cbPciLength = 0;
	sRecvPci.dwProtocol = SCARD_PROTOCOL_T0;
	sRecvPci.cbPciLength = 0;

	dwSendLength = data - s;
	dwRecvLength = apdu->resplen + 2;
	if (dwRecvLength > 255)		/* FIXME: PC/SC Lite quirk */
		dwRecvLength = 255;
	if (card->ctx->debug >= 5) {
		char buf[2048];
		
		sc_hex_dump(card->ctx, s, (size_t) dwSendLength, buf, sizeof(buf));
		debug(card->ctx, "Sending %d bytes (resp. %d bytes):\n%s",
			dwSendLength, dwRecvLength, buf);
	}
	rv = SCardTransmit(card->pcsc_card, &sSendPci, s, dwSendLength,
			   &sRecvPci, r, &dwRecvLength);
	if (rv != SCARD_S_SUCCESS) {
		switch (rv) {
		case SCARD_W_REMOVED_CARD:
			return SC_ERROR_CARD_REMOVED;
		case SCARD_W_RESET_CARD:
			return SC_ERROR_CARD_RESET;
		case SCARD_E_NOT_TRANSACTED:
			if (sc_detect_card(card->ctx, card->reader) != 1)
				return SC_ERROR_CARD_REMOVED;
			return SC_ERROR_TRANSMIT_FAILED;
		default:
			error(card->ctx, "SCardTransmit failed: %s\n", pcsc_stringify_error(rv));
			return SC_ERROR_TRANSMIT_FAILED;
		}
	}
	if (dwRecvLength < 2)
		return SC_ERROR_ILLEGAL_RESPONSE;
	apdu->sw1 = (unsigned int) r[dwRecvLength-2];
	apdu->sw2 = (unsigned int) r[dwRecvLength-1];
	dwRecvLength -= 2;
	if ((size_t) dwRecvLength > apdu->resplen)
		dwRecvLength = (DWORD) apdu->resplen;
	else
		apdu->resplen = (size_t) dwRecvLength;
	if (dwRecvLength > 0)
		memcpy(apdu->resp, r, (size_t) dwRecvLength);

	return 0;
}

int sc_transmit_apdu(struct sc_card *card, struct sc_apdu *apdu)
{
	int r;
	size_t orig_resplen;
	
	assert(card != NULL && apdu != NULL);
	SC_FUNC_CALLED(card->ctx, 4);
	orig_resplen = apdu->resplen;
	r = sc_check_apdu(card->ctx, apdu);
	SC_TEST_RET(card->ctx, r, "APDU sanity check failed");
	r = sc_lock(card);
	SC_TEST_RET(card->ctx, r, "sc_lock() failed");
	r = sc_transceive_t0(card, apdu);
	if (r != 0) {
		sc_unlock(card);
		SC_TEST_RET(card->ctx, r, "transceive_t0() failed");
	}
	if (card->ctx->debug >= 5) {
		char buf[2048];

		buf[0] = '\0';
		if (apdu->resplen > 0) {
			sc_hex_dump(card->ctx, apdu->resp, apdu->resplen,
				    buf, sizeof(buf));
		}
		debug(card->ctx, "Received %d bytes (SW1=%02X SW2=%02X)\n%s",
		      apdu->resplen, apdu->sw1, apdu->sw2, buf);
	}
	if (apdu->sw1 == 0x6C && apdu->resplen == 0) {
		apdu->resplen = orig_resplen;
		apdu->le = apdu->sw2;
		r = sc_transceive_t0(card, apdu);
		if (r != 0) {
			sc_unlock(card);
			SC_TEST_RET(card->ctx, r, "transceive_t0() failed");
		}
	}
	if (apdu->sw1 == 0x61 && apdu->resplen == 0) {
		struct sc_apdu rspapdu;
		BYTE rsp[SC_MAX_APDU_BUFFER_SIZE];

		if (orig_resplen == 0) {
			apdu->sw1 = 0x90;	/* FIXME: should we do this? */
			apdu->sw2 = 0;
			sc_unlock(card);
			return 0;
		}
		
		sc_format_apdu(card, &rspapdu, SC_APDU_CASE_2_SHORT,
			       0xC0, 0, 0);
		rspapdu.le = (size_t) apdu->sw2;
		rspapdu.resp = rsp;
		rspapdu.resplen = (size_t) apdu->sw2;
		r = sc_transceive_t0(card, &rspapdu);
		if (r != 0) {
			error(card->ctx, "error while getting response: %s\n",
			      sc_strerror(r));
			sc_unlock(card);
			return r;
		}
		if (card->ctx->debug >= 5) {
			char buf[2048];
			buf[0] = 0;
			if (rspapdu.resplen) {
				sc_hex_dump(card->ctx, rspapdu.resp,
					    rspapdu.resplen,
					    buf, sizeof(buf));
			}
			debug(card->ctx, "Response %d bytes (SW1=%02X SW2=%02X)\n%s",
			      rspapdu.resplen, rspapdu.sw1, rspapdu.sw2, buf);
		}
		if (rspapdu.resplen) {
			size_t c = rspapdu.resplen;
			
			if (c > orig_resplen)
				c = orig_resplen;
			memcpy(apdu->resp, rspapdu.resp, c);
			apdu->resplen = c;
		}
		apdu->sw1 = rspapdu.sw1;
		apdu->sw2 = rspapdu.sw2;
	}
	sc_unlock(card);
	return 0;
}

void sc_format_apdu(struct sc_card *card, struct sc_apdu *apdu,
		   int cse, int ins, int p1, int p2)
{
	assert(card != NULL && apdu != NULL);
	memset(apdu, 0, sizeof(*apdu));
	apdu->cla = (u8) card->cla;
	apdu->cse = cse;
	apdu->ins = (u8) ins;
	apdu->p1 = (u8) p1;
	apdu->p2 = (u8) p2;
	
	return;
}

int sc_connect_card(struct sc_context *ctx,
		    int reader, struct sc_card **card_out)
{
	struct sc_card *card;
	DWORD active_proto;
	SCARDHANDLE card_handle;
	SCARD_READERSTATE_A rgReaderStates[SC_MAX_READERS];
	LONG rv;
	int i;

	assert(card_out != NULL);
	SC_FUNC_CALLED(ctx, 1);
	if (reader >= ctx->reader_count || reader < 0)
		SC_FUNC_RETURN(ctx, 1, SC_ERROR_OBJECT_NOT_FOUND);
	
	rgReaderStates[0].szReader = ctx->readers[reader];
	rgReaderStates[0].dwCurrentState = SCARD_STATE_UNAWARE;
	rv = SCardGetStatusChange(ctx->pcsc_ctx, 0, rgReaderStates, 1);
	if (rv != 0) {
		error(ctx, "SCardGetStatusChange failed: %s\n", pcsc_stringify_error(rv));
		SC_FUNC_RETURN(ctx, 1, SC_ERROR_RESOURCE_MANAGER);	/* FIXME */
	}
	if (!(rgReaderStates[0].dwEventState & SCARD_STATE_PRESENT))
		SC_FUNC_RETURN(ctx, 1, SC_ERROR_CARD_NOT_PRESENT);

	card = malloc(sizeof(struct sc_card));
	if (card == NULL)
		SC_FUNC_RETURN(ctx, 1, SC_ERROR_OUT_OF_MEMORY);
	memset(card, 0, sizeof(struct sc_card));
	rv = SCardConnect(ctx->pcsc_ctx, ctx->readers[reader],
			  SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0,
			  &card_handle, &active_proto);
	if (rv != 0) {
		error(ctx, "SCardConnect failed: %s\n", pcsc_stringify_error(rv));
		free(card);
		return -1;	/* FIXME */
	}
	card->reader = reader;
	card->ctx = ctx;
	card->pcsc_card = card_handle;
	card->lock_count = 0;
	i = rgReaderStates[0].cbAtr;
	if (i >= SC_MAX_ATR_SIZE)
		i = SC_MAX_ATR_SIZE;
	memcpy(card->atr, rgReaderStates[0].rgbAtr, i);
	card->atr_len = i;

	for (i = 0; ctx->card_drivers[i] != NULL; i++) {
                const struct sc_card_driver *drv = ctx->card_drivers[i];
		const struct sc_card_operations *ops = drv->ops;
		int r;
		
		if (ctx->debug >= 3)
			debug(ctx, "trying driver: %s\n", drv->name);
		if (ops == NULL || ops->match_card == NULL)
			continue;
		if (ops->match_card(card) != 1)
			continue;
		if (ctx->debug >= 3)
			debug(ctx, "matched: %s\n", drv->name);
		r = ops->init(card);
		card->ops = ops;
		card->driver = drv;
		if (r) {
			error(ctx, "driver '%s' init() failed: %s\n", drv->name,
			      sc_strerror(r));
			if (r == SC_ERROR_INVALID_CARD) {
				card->ops = NULL;
				card->driver = NULL;
				continue;
			}
			free(card);
			return r;
		}
		break;
	}
	if (card->ops == NULL) {
		error(ctx, "unable to find driver for inserted card\n");
                free(card);
		return SC_ERROR_INVALID_CARD;
	}
	pthread_mutex_init(&card->mutex, NULL);
	card->magic = SC_CARD_MAGIC;
	*card_out = card;

	SC_FUNC_RETURN(ctx, 1, 0);
}

int sc_disconnect_card(struct sc_card *card)
{
	struct sc_context *ctx;
	assert(sc_card_valid(card));
	ctx = card->ctx;
	SC_FUNC_CALLED(ctx, 1);
	assert(card->lock_count == 0);
	if (card->ops->finish) {
		int r = card->ops->finish(card);
		if (r)
			error(card->ctx, "driver finish() failed: %s\n",
			      sc_strerror(r));
	}
	SCardDisconnect(card->pcsc_card, SCARD_LEAVE_CARD);
	pthread_mutex_destroy(&card->mutex);
	free(card);
	SC_FUNC_RETURN(ctx, 1, 0);
}

/* internal lock function -- should make sure that the card is exclusively
 * in our use */
static int _sc_lock_int(struct sc_card *card)
{
	long rv;

	rv = SCardBeginTransaction(card->pcsc_card);
	
	if (rv != SCARD_S_SUCCESS) {
		error(card->ctx, "SCardBeginTransaction failed: %s\n", pcsc_stringify_error(rv));
		return _sc_pcscret_to_error(rv);
	}
	return 0;
}

int sc_lock(struct sc_card *card)
{
	int r = 0;
	
	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	pthread_mutex_lock(&card->mutex);
	if (card->lock_count == 0)
		r = _sc_lock_int(card);
	if (r == 0)
		card->lock_count++;
	pthread_mutex_unlock(&card->mutex);
	SC_FUNC_RETURN(card->ctx, 2, r);
}

/* internal unlock function */
static int _sc_unlock_int(struct sc_card *card)
{
	long rv;
	
	rv = SCardEndTransaction(card->pcsc_card, SCARD_LEAVE_CARD);
	if (rv != SCARD_S_SUCCESS) {
		error(card->ctx, "SCardEndTransaction failed: %s\n", pcsc_stringify_error(rv));
		return -1;
	}
	return 0;
}

int sc_unlock(struct sc_card *card)
{
	int r = 0;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	pthread_mutex_lock(&card->mutex);
	card->lock_count--;
	assert(card->lock_count >= 0);
	if (card->lock_count == 0)
		r = _sc_unlock_int(card);
	pthread_mutex_unlock(&card->mutex);
	SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_list_files(struct sc_card *card, u8 *buf, int buflen)
{
	struct sc_apdu apdu;
	int r;

	SC_FUNC_CALLED(card->ctx, 2);	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xAA, 0, 0);
	apdu.resp = buf;
	apdu.resplen = buflen;
	apdu.le = 0;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.resplen == 0)
		return sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2);
	return apdu.resplen;
}

static int construct_fci(const struct sc_file *file, u8 *out, int *outlen)
{
	u8 *p = out;
	u8 buf[32];
	
	*p++ = 0x6F;
	p++;
	
	buf[0] = (file->size >> 8) & 0xFF;
	buf[1] = file->size & 0xFF;
	sc_asn1_put_tag(0x81, buf, 2, p, 16, &p);
	buf[0] = file->shareable ? 0x40 : 0;
	buf[0] |= (file->type & 7) << 3;
	buf[0] |= file->ef_structure & 7;
	sc_asn1_put_tag(0x82, buf, 1, p, 16, &p);
	buf[0] = (file->id >> 8) & 0xFF;
	buf[1] = file->id & 0xFF;
	sc_asn1_put_tag(0x83, buf, 2, p, 16, &p);
	/* 0x84 = DF name */
	if (file->prop_attr_len) {
		memcpy(buf, file->prop_attr, file->prop_attr_len);
		sc_asn1_put_tag(0x85, buf, file->prop_attr_len, p, 18, &p);
	}
	if (file->sec_attr_len) {
		memcpy(buf, file->sec_attr, file->sec_attr_len);
		sc_asn1_put_tag(0x86, buf, file->sec_attr_len, p, 18, &p);
	}
	*p++ = 0xDE;
	*p++ = 0;
	*outlen = p - out;
	out[1] = p - out - 2;
	return 0;
}

int sc_create_file(struct sc_card *card, const struct sc_file *file)
{
	int r, len;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, 1);
	len = SC_MAX_APDU_BUFFER_SIZE;
	r = construct_fci(file, sbuf, &len);
	SC_TEST_RET(card->ctx, r, "construct_fci() failed");
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0x00, 0x00);
	apdu.lc = len;
	apdu.datalen = len;
	apdu.data = sbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.resp = rbuf;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	SC_FUNC_RETURN(card->ctx, 1, sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2));
}

int sc_delete_file(struct sc_card *card, int file_id)
{
	int r;
	u8 sbuf[2];
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, 1);
	sbuf[0] = (file_id >> 8) & 0xFF;
	sbuf[1] = file_id & 0xFF;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, 0x00, 0x00);
	apdu.lc = 2;
	apdu.datalen = 2;
	apdu.data = sbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.resp = rbuf;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.resplen != 0)
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_ILLEGAL_RESPONSE);
	SC_FUNC_RETURN(card->ctx, 1, sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2));
}

int sc_read_binary(struct sc_card *card, unsigned int idx,
		   unsigned char *buf, size_t count, unsigned long flags)
{
#define RB_BUF_SIZE 250
	int r;

	assert(card != NULL && card->ops != NULL && buf != NULL);
	if (card->ctx->debug >= 2)
		debug(card->ctx, "sc_read_binary: %d bytes at index %d\n", count, idx);
	if (count > RB_BUF_SIZE) {
		int bytes_read = 0;
		unsigned char *p = buf;

		if (card->ops->read_binary_large != NULL) {
			r = card->ops->read_binary_large(card, idx, buf, count, flags);
			SC_FUNC_RETURN(card->ctx, 2, r);
		}
                /* no read_binary_large support... */
		r = sc_lock(card);
		SC_TEST_RET(card->ctx, r, "sc_lock() failed");
		while (count > 0) {
			int n = count > RB_BUF_SIZE ? RB_BUF_SIZE : count;
			r = sc_read_binary(card, idx, p, n, flags);
			if (r < 0) {
				sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "sc_read_binary() failed");
			}
			p += r;
			idx += r;
			bytes_read += r;
			count -= r;
			if (r == 0) {
				sc_unlock(card);
				SC_FUNC_RETURN(card->ctx, 2, bytes_read);
			}
		}
		sc_unlock(card);
		SC_FUNC_RETURN(card->ctx, 2, bytes_read);
	}
	if (card->ops->read_binary == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->read_binary(card, idx, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_select_file(struct sc_card *card,
		   const struct sc_path *in_path,
		   struct sc_file *file)
{
	int r;

	assert(card != NULL && in_path != NULL);
	if (card->ctx->debug >= 2) {
		char line[128], *linep = line;

		linep += sprintf(linep, "called with type %d, path ", in_path->type);
		for (r = 0; r < in_path->len; r++) {
			sprintf(linep, "%02X", in_path->value[r]);
			linep += 2;
		}
		strcpy(linep, "\n");
		debug(card->ctx, line);
	}
	if (in_path->len > SC_MAX_PATH_SIZE)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
        if (card->ops->select_file == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->select_file(card, in_path, file);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_get_challenge(struct sc_card *card, u8 *rnd, size_t len)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->get_challenge == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->get_challenge(card, rnd, len);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

int sc_read_record(struct sc_card *card, unsigned int rec_nr, u8 *buf,
		   size_t count, unsigned long flags)
{
	int r;

	assert(card != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
        if (card->ops->read_record == NULL)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_SUPPORTED);
	r = card->ops->read_record(card, rec_nr, buf, count, flags);
        SC_FUNC_RETURN(card->ctx, 2, r);
}

inline int sc_card_valid(const struct sc_card *card) {
#ifndef NDEBUG
	assert(card != NULL);
#endif
	return card->magic == SC_CARD_MAGIC;
}
