/*
 * Copyright 2012-2016 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the standard MIT license.  See COPYING for more details.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif

#include <blkmaker.h>
#include <blktemplate.h>

#include "private.h"

static inline
void my_htole32(unsigned char *buf, uint32_t n) {
	buf[0] = (n >>  0) % 256;
	buf[1] = (n >>  8) % 256;
	buf[2] = (n >> 16) % 256;
	buf[3] = (n >> 24) % 256;
}

static inline
void my_htole64(unsigned char *buf, uint64_t n) {
	for (int i = 0; i < 8; ++i)
		buf[i] = (n >>  (8*i)) & 0xff;
}


bool (*blkmk_sha256_impl)(void *, const void *, size_t) = NULL;

bool _blkmk_dblsha256(void *hash, const void *data, size_t datasz) {
	return blkmk_sha256_impl(hash, data, datasz) && blkmk_sha256_impl(hash, hash, 32);
}

#define dblsha256 _blkmk_dblsha256

static
size_t varintDecode(const uint8_t *p, size_t size, uint64_t *n)
{
	if (size > 8 && p[0] == 0xff)
	{
		*n = upk_u64le(p, 1);
		return 9;
	}
	if (size > 4 && p[0] == 0xfe)
	{
		*n = upk_u32le(p, 1);
		return 5;
	}
	if (size > 2 && p[0] == 0xfd)
	{
		*n = upk_u16le(p, 1);
		return 3;
	}
	if (size > 0 && p[0] <= 0xfc)
	{
		*n = p[0];
		return 1;
	}
	return 0;
}

#define max_varint_size (9)

static
char varintEncode(unsigned char *out, uint64_t n) {
	if (n < 0xfd)
	{
		out[0] = n;
		return 1;
	}
	char L;
	if (n <= 0xffff)
	{
		out[0] = '\xfd';
		L = 3;
	}
	else
	if (n <= 0xffffffff)
	{
		out[0] = '\xfe';
		L = 5;
	}
	else
	{
		out[0] = '\xff';
		L = 9;
	}
	for (unsigned char i = 1; i < L; ++i)
		out[i] = (n >> ((i - 1) * 8)) % 256;
	return L;
}

uint64_t blkmk_init_generation3(blktemplate_t * const tmpl, const void * const script, const size_t scriptsz, bool * const inout_newcb) {
	if (tmpl->cbtxn && !(*inout_newcb && (tmpl->mutations & BMM_GENERATE)))
	{
		*inout_newcb = false;
		return 0;
	}
	
	*inout_newcb = true;
	
	if (scriptsz >= 0xfd)
		return 0;
	
	unsigned char *data = malloc(168 + scriptsz);
	size_t off = 0;
	if (!data)
		return 0;
	
	memcpy(&data[0],
		"\x01\0\0\0"  // txn ver
		"\x01"        // input count
			"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"  // prevout
			"\xff\xff\xff\xff"  // index (-1)
			"\x02"              // scriptSig length
			// height serialization length (set later)
		, 42);
	off += 43;
	
	blkheight_t h = tmpl->height;
	while (h > 127)
	{
		++data[41];
		data[off++] = h & 0xff;
		h >>= 8;
	}
	data[off++] = h;
	data[42] = data[41] - 1;
	
	if (tmpl->aux_count)
	{
		unsigned auxsz = off++;
		data[auxsz] = 0;
		++data[41];
		
		for (unsigned i = 0; i < tmpl->aux_count; ++i)
		{
			struct blkaux_t * const aux = &tmpl->auxs[i];
			if ((size_t)data[41] + aux->datasz > 100)
			{
				free(data);
				return 0;
			}
			memcpy(&data[off], tmpl->auxs[i].data, aux->datasz);
			data[41] += aux->datasz;
			data[auxsz] += aux->datasz;
			off += aux->datasz;
		}
	}
	
	memcpy(&data[off],
			"\xff\xff\xff\xff"  // sequence
		"\x01"        // output count
		, 5);
	off += 5;
	my_htole64(&data[off], tmpl->cbvalue);
	off += 8;
	data[off++] = scriptsz;
	memcpy(&data[off], script, scriptsz);
	off += scriptsz;
	memset(&data[off], 0, 4);  // lock time
	off += 4;
	
	struct blktxn_t *txn = calloc(1, sizeof(*tmpl->cbtxn));
	if (!txn)
	{
		free(data);
		return 0;
	}
	
	txn->data = data;
	txn->datasz = off;
	
	if (tmpl->cbtxn)
	{
		_blktxn_free(tmpl->cbtxn);
		free(tmpl->cbtxn);
	}
	tmpl->cbtxn = txn;
	
	tmpl->mutations |= BMM_CBAPPEND | BMM_CBSET | BMM_GENERATE;
	
	return tmpl->cbvalue;
}

uint64_t blkmk_init_generation2(blktemplate_t *tmpl, void *script, size_t scriptsz, bool *out_newcb) {
	bool tmp;
	if (!out_newcb)
		out_newcb = &tmp;
	*out_newcb = false;
	return blkmk_init_generation3(tmpl, script, scriptsz, out_newcb);
}

uint64_t blkmk_init_generation(blktemplate_t *tmpl, void *script, size_t scriptsz) {
	return blkmk_init_generation2(tmpl, script, scriptsz, NULL);
}

static
bool blkmk_hash_transactions(blktemplate_t * const tmpl)
{
	for (unsigned long i = 0; i < tmpl->txncount; ++i)
	{
		struct blktxn_t * const txn = &tmpl->txns[i];
		if (txn->hash_)
			continue;
		txn->hash_ = malloc(sizeof(*txn->hash_));
		if (!dblsha256(txn->hash_, txn->data, txn->datasz))
		{
			free(txn->hash_);
			return false;
		}
	}
	return true;
}

static
bool blkmk_build_merkle_branches(blktemplate_t * const tmpl)
{
	int branchcount, i;
	libblkmaker_hash_t *branches;
	
	if (tmpl->_mrklbranch)
		return true;
	
	if (!blkmk_hash_transactions(tmpl))
		return false;
	
	branchcount = blkmk_flsl(tmpl->txncount);
	if (!branchcount)
	{
		tmpl->_mrklbranchcount = 0;
		tmpl->_mrklbranch = NULL;
		return true;
	}
	
	branches = malloc(branchcount * sizeof(*branches));
	
	size_t hashcount = tmpl->txncount + 1;
	unsigned char hashes[(hashcount + 1) * 32];
	
	for (i = 0; i < tmpl->txncount; ++i)
	{
		struct blktxn_t * const txn = &tmpl->txns[i];
		txnhash_t * const txid = txn->txid ? txn->txid : txn->hash_;
		memcpy(&hashes[0x20 * (i + 1)], txid, 0x20);
	}
	
	for (i = 0; i < branchcount; ++i)
	{
		memcpy(&branches[i], &hashes[0x20], 0x20);
		if (hashcount % 2)
		{
			memcpy(&hashes[32 * hashcount], &hashes[32 * (hashcount - 1)], 32);
			++hashcount;
		}
		for (size_t i = 2; i < hashcount; i += 2)
			// This is where we overlap input and output, on the first pair
			if (!dblsha256(&hashes[i / 2 * 32], &hashes[32 * i], 64))
			{
				free(branches);
				return false;
			}
		hashcount /= 2;
	}
	
	tmpl->_mrklbranch = branches;
	tmpl->_mrklbranchcount = branchcount;
	
	return true;
}

static
bool build_merkle_root(unsigned char *mrklroot_out, blktemplate_t *tmpl, unsigned char *cbtxndata, size_t cbtxndatasz) {
	int i;
	libblkmaker_hash_t hashes[0x40];
	
	if (!blkmk_build_merkle_branches(tmpl))
		return false;
	
	if (!dblsha256(&hashes[0], cbtxndata, cbtxndatasz))
		return false;
	
	for (i = 0; i < tmpl->_mrklbranchcount; ++i)
	{
		memcpy(&hashes[1], tmpl->_mrklbranch[i], 0x20);
		// This is where we overlap input and output, on the first pair
		if (!dblsha256(&hashes[0], &hashes[0], 0x40))
			return false;
	}
	
	memcpy(mrklroot_out, &hashes[0], 32);
	
	return true;
}

static
bool _blkmk_calculate_witness_mrklroot(blktemplate_t * const tmpl, libblkmaker_hash_t * const out, bool * const witness_needed) {
	if (!blkmk_hash_transactions(tmpl))
		return false;
	
	// Step 1: Populate hashes with the witness hashes for all transactions
	size_t hashcount = tmpl->txncount + 1;
	libblkmaker_hash_t hashes[hashcount + 1];  // +1 for when the last needs duplicating
	memset(&hashes[0], 0, sizeof(hashes[0]));  // Gen tx gets a null entry
	*witness_needed = false;
	for (unsigned long i = 0; i < tmpl->txncount; ++i) {
		struct blktxn_t * const txn = &tmpl->txns[i];
		if (txn->txid && memcmp(txn->hash_, txn->txid, sizeof(*txn->txid))) {
			*witness_needed = true;
		}
		memcpy(&hashes[i + 1], txn->hash_, sizeof(*hashes));
	}
	if (!*witness_needed)
		return true;
	
	// Step 2: Reduce it to a merkle root
	for ( ; hashcount > 1 ; hashcount /= 2) {
		if (hashcount % 1 == 1) {
			// Odd number, duplicate the last
			memcpy(&hashes[hashcount], &hashes[hashcount - 1], sizeof(*hashes));
			++hashcount;
		}
		for (size_t i = 0; i < hashcount; i += 2) {
			// We overlap input and output here, on the first pair
			if (!dblsha256(&hashes[i / 2], &hashes[i], sizeof(*hashes) * 2)) {
				return false;
			}
		}
	}
	
	memcpy(out, hashes, sizeof(*out));
	return true;
}

static
bool _blkmk_witness_mrklroot(blktemplate_t * const tmpl) {
	if (tmpl->_calculated_witness) {
		// Already calculated
		return true;
	}
	tmpl->_witnessmrklroot = malloc(sizeof(libblkmaker_hash_t));
	if (!tmpl->_witnessmrklroot) {
		return false;
	}
	bool witness_needed;
	if (!_blkmk_calculate_witness_mrklroot(tmpl, tmpl->_witnessmrklroot, &witness_needed)) {
		free(tmpl->_witnessmrklroot);
		tmpl->_witnessmrklroot = NULL;
		return false;
	}
	if (!witness_needed) {
		free(tmpl->_witnessmrklroot);
		tmpl->_witnessmrklroot = NULL;
	}
	tmpl->_calculated_witness = true;
	return true;
}

static const int cbScriptSigLen = 4 + 1 + 36;

static
bool _blkmk_append_cb(blktemplate_t * const tmpl, void * const vout, const void * const append, const size_t appendsz, size_t * const appended_at_offset) {
	unsigned char *out = vout;
	unsigned char *in = tmpl->cbtxn->data;
	size_t insz = tmpl->cbtxn->datasz;
	
	if (in[cbScriptSigLen] > 100 - appendsz)
		return false;
	
	int cbPostScriptSig = cbScriptSigLen + 1 + in[cbScriptSigLen];
	if (appended_at_offset)
		*appended_at_offset = cbPostScriptSig;
	unsigned char *outPostScriptSig = &out[cbPostScriptSig];
	void *outExtranonce = (void*)outPostScriptSig;
	outPostScriptSig += appendsz;
	
	if (out != in)
	{
		memcpy(out, in, cbPostScriptSig+1);
		memcpy(outPostScriptSig, &in[cbPostScriptSig], insz - cbPostScriptSig);
	}
	else
		memmove(outPostScriptSig, &in[cbPostScriptSig], insz - cbPostScriptSig);
	
	out[cbScriptSigLen] += appendsz;
	memcpy(outExtranonce, append, appendsz);
	
	return true;
}

ssize_t blkmk_append_coinbase_safe2(blktemplate_t * const tmpl, const void * const append, const size_t appendsz, int extranoncesz, const bool merkle_only)
{
	if (!(tmpl->mutations & (BMM_CBAPPEND | BMM_CBSET)))
		return -1;
	
	size_t datasz = tmpl->cbtxn->datasz;
	if (!merkle_only)
	{
		if (extranoncesz < sizeof(unsigned int))
			extranoncesz = sizeof(unsigned int);
		else
		if (extranoncesz == sizeof(unsigned int))
			++extranoncesz;
	}
	size_t availsz = 100 - extranoncesz - tmpl->cbtxn->data[cbScriptSigLen];
	if (appendsz > availsz)
		return availsz;
	
	void *newp = realloc(tmpl->cbtxn->data, datasz + appendsz);
	if (!newp)
		return -2;
	
	tmpl->cbtxn->data = newp;
	if (!_blkmk_append_cb(tmpl, newp, append, appendsz, NULL))
		return -3;
	tmpl->cbtxn->datasz += appendsz;
	
	return availsz;
}

ssize_t blkmk_append_coinbase_safe(blktemplate_t * const tmpl, const void * const append, const size_t appendsz) {
	return blkmk_append_coinbase_safe2(tmpl, append, appendsz, 0, false);
}

bool _blkmk_extranonce(blktemplate_t *tmpl, void *vout, unsigned int workid, size_t *offs) {
	unsigned char *in = tmpl->cbtxn->data;
	size_t insz = tmpl->cbtxn->datasz;
	
	if (!workid)
	{
		memcpy(vout, in, insz);
		*offs += insz;
		return true;
	}
	
	if (!_blkmk_append_cb(tmpl, vout, &workid, sizeof(workid), NULL))
		return false;
	
	*offs += insz + sizeof(workid);
	
	return true;
}

static const unsigned char witness_magic[] = { 0x6a /* OP_RETURN */, 0x24, 0xaa, 0x21, 0xa9, 0xed };
#define commitment_spk_size (sizeof(witness_magic) + sizeof(libblkmaker_hash_t) /* witness mrklroot */)
#define commitment_txout_size (8 /* value */ + 1 /* scriptPubKey length */ + commitment_spk_size)
static const size_t max_witness_commitment_insert = max_varint_size + commitment_txout_size - 1;

static
bool _blkmk_insert_witness_commitment(blktemplate_t * const tmpl, unsigned char * const gentxdata, size_t * const gentxsize) {
	if (!_blkmk_witness_mrklroot(tmpl)) {
		return false;
	}
	if (!tmpl->_witnessmrklroot) {
		// No commitment needed
		return true;
	}
	
	if (cbScriptSigLen >= *gentxsize) {
		return false;
	}
	const uint8_t coinbasesz = gentxdata[cbScriptSigLen];
	const size_t offset_of_txout_count = cbScriptSigLen + coinbasesz + sizeof(coinbasesz) + 4 /* nSequence */;
	if (offset_of_txout_count >= *gentxsize) {
		return false;
	}
	uint64_t txout_count;
	const size_t in_txout_count_size = varintDecode(&gentxdata[offset_of_txout_count], *gentxsize - offset_of_txout_count, &txout_count);
	if (!in_txout_count_size) {
		return false;
	}
	++txout_count;
	unsigned char insertbuf[max_varint_size + commitment_txout_size];
	const size_t out_txout_count_size = varintEncode(insertbuf, txout_count);
	unsigned char * const commitment_txout = &insertbuf[out_txout_count_size];
	memset(commitment_txout, 0, 8);  // value
	commitment_txout[8] = commitment_spk_size;
	memcpy(&commitment_txout[9], witness_magic, sizeof(witness_magic));
	memcpy(&commitment_txout[9 + sizeof(witness_magic)], tmpl->_witnessmrklroot, sizeof(*tmpl->_witnessmrklroot));
	
	// TODO: Put the new txout at the end to reduce movement
	const size_t offset_of_txout_data = (offset_of_txout_count + in_txout_count_size);
	const size_t new_offset_of_preexisting_txout_data = (offset_of_txout_count + out_txout_count_size + commitment_txout_size);
	const size_t length_of_preexisting_txout_data_to_end_of_gentx = *gentxsize - offset_of_txout_data;
	memmove(&gentxdata[new_offset_of_preexisting_txout_data], &gentxdata[offset_of_txout_data], length_of_preexisting_txout_data_to_end_of_gentx);
	const size_t movement_delta = new_offset_of_preexisting_txout_data - offset_of_txout_data;
	*gentxsize += movement_delta;
	
	const size_t insertbuf_len = out_txout_count_size + commitment_txout_size;
	memcpy(&gentxdata[offset_of_txout_count], insertbuf, insertbuf_len);
	
	return true;
}

static
void blkmk_set_times(blktemplate_t *tmpl, void * const out_hdrbuf, const time_t usetime, int16_t * const out_expire, const bool can_roll_ntime)
{
	double time_passed = difftime(usetime, tmpl->_time_rcvd);
	blktime_t timehdr = tmpl->curtime + time_passed;
	if (timehdr > tmpl->maxtime)
		timehdr = tmpl->maxtime;
	my_htole32(out_hdrbuf, timehdr);
	if (out_expire)
	{
		*out_expire = tmpl->expires - time_passed - 1;
		
		if (can_roll_ntime)
		{
			// If the caller can roll the time header, we need to expire before reaching the maxtime
			int16_t maxtime_expire_limit = (tmpl->maxtime - timehdr) + 1;
			if (*out_expire > maxtime_expire_limit)
				*out_expire = maxtime_expire_limit;
		}
	}
}

bool blkmk_sample_data_(blktemplate_t * const tmpl, uint8_t * const cbuf, const unsigned int dataid) {
	my_htole32(&cbuf[0], tmpl->version);
	memcpy(&cbuf[4], &tmpl->prevblk, 32);
	
	unsigned char cbtxndata[tmpl->cbtxn->datasz + sizeof(dataid) + max_witness_commitment_insert];
	size_t cbtxndatasz = 0;
	if (!_blkmk_extranonce(tmpl, cbtxndata, dataid, &cbtxndatasz))
		return false;
	if (!_blkmk_insert_witness_commitment(tmpl, cbtxndata, &cbtxndatasz)) {
		return false;
	}
	if (!build_merkle_root(&cbuf[36], tmpl, cbtxndata, cbtxndatasz))
		return false;
	
	my_htole32(&cbuf[0x44], tmpl->curtime);
	memcpy(&cbuf[72], &tmpl->diffbits, 4);
	
	return true;
}

size_t blkmk_get_data(blktemplate_t *tmpl, void *buf, size_t bufsz, time_t usetime, int16_t *out_expire, unsigned int *out_dataid) {
	if (!(blkmk_time_left(tmpl, usetime) && blkmk_work_left(tmpl) && tmpl->cbtxn))
		return 0;
	if (bufsz < 76)
		return 76;
	
	unsigned char *cbuf = buf;
	
	*out_dataid = tmpl->next_dataid++;
	if (!blkmk_sample_data_(tmpl, cbuf, *out_dataid))
		return 0;
	blkmk_set_times(tmpl, &cbuf[68], usetime, out_expire, false);
	
	return 76;
}

bool blkmk_get_mdata(blktemplate_t * const tmpl, void * const buf, const size_t bufsz, const time_t usetime, int16_t * const out_expire, void * const _out_cbtxn, size_t * const out_cbtxnsz, size_t * const cbextranonceoffset, int * const out_branchcount, void * const _out_branches, size_t extranoncesz, const bool can_roll_ntime)
{
	if (!(true
		&& blkmk_time_left(tmpl, usetime)
		&& tmpl->cbtxn
		&& blkmk_build_merkle_branches(tmpl)
		&& bufsz >= 76
	))
		return false;
	
	if (extranoncesz == sizeof(unsigned int))
		// Avoid overlapping with blkmk_get_data use
		++extranoncesz;
	
	void ** const out_branches = _out_branches;
	void ** const out_cbtxn = _out_cbtxn;
	unsigned char *cbuf = buf;
	
	my_htole32(&cbuf[0], tmpl->version);
	memcpy(&cbuf[4], &tmpl->prevblk, 32);
	
	*out_cbtxnsz = tmpl->cbtxn->datasz + extranoncesz;
	*out_cbtxn = malloc(*out_cbtxnsz + max_witness_commitment_insert);
	if (!*out_cbtxn)
		return false;
	unsigned char dummy[extranoncesz];
	memset(dummy, 0, extranoncesz);
	if (!_blkmk_append_cb(tmpl, *out_cbtxn, dummy, extranoncesz, cbextranonceoffset))
	{
		free(*out_cbtxn);
		return false;
	}
	if (!_blkmk_insert_witness_commitment(tmpl, *out_cbtxn, out_cbtxnsz)) {
		free(*out_cbtxn);
		return false;
	}
	
	blkmk_set_times(tmpl, &cbuf[68], usetime, out_expire, can_roll_ntime);
	memcpy(&cbuf[72], &tmpl->diffbits, 4);
	
	*out_branchcount = tmpl->_mrklbranchcount;
	const size_t branches_bytesz = (sizeof(libblkmaker_hash_t) * tmpl->_mrklbranchcount);
	*out_branches = malloc(branches_bytesz);
	if (!*out_branches)
	{
		free(*out_cbtxn);
		return false;
	}
	memcpy(*out_branches, tmpl->_mrklbranch, branches_bytesz);
	
	return true;
}

blktime_diff_t blkmk_time_left(const blktemplate_t *tmpl, time_t nowtime) {
	double age = difftime(nowtime, tmpl->_time_rcvd);
	if (age >= tmpl->expires)
		return 0;
	return tmpl->expires - age;
}

unsigned long blkmk_work_left(const blktemplate_t *tmpl) {
	if (!tmpl->version)
		return 0;
	if (!(tmpl->mutations & (BMM_CBAPPEND | BMM_CBSET)))
		return 1;
	return UINT_MAX - tmpl->next_dataid;
	return BLKMK_UNLIMITED_WORK_COUNT;
}

char *blkmk_assemble_submission_(blktemplate_t * const tmpl, const unsigned char * const data, const unsigned int dataid, blknonce_t nonce, const bool foreign)
{
	unsigned char blk[80 + 8 + 1000000];
	memcpy(blk, data, 76);
	nonce = htonl(nonce);
	memcpy(&blk[76], &nonce, 4);
	size_t offs = 80;
	
	if (foreign || (!(tmpl->mutations & BMAb_TRUNCATE && !dataid)))
	{
		offs += varintEncode(&blk[offs], 1 + tmpl->txncount);
		
		size_t cbtxnlen = 0;
		if (!_blkmk_extranonce(tmpl, &blk[offs], dataid, &cbtxnlen))
			return NULL;
		if (!_blkmk_insert_witness_commitment(tmpl, &blk[offs], &cbtxnlen)) {
			return NULL;
		}
		offs += cbtxnlen;
		
		if (foreign || !(tmpl->mutations & BMAb_COINBASE))
			for (unsigned long i = 0; i < tmpl->txncount; ++i)
			{
				memcpy(&blk[offs], tmpl->txns[i].data, tmpl->txns[i].datasz);
				offs += tmpl->txns[i].datasz;
			}
	}
	
	char *blkhex = malloc((offs * 2) + 1);
	_blkmk_bin2hex(blkhex, blk, offs);
	
	return blkhex;
}
