// <COPYRIGHT_TAG>

#include <stdint.h>
#include "inflate.h"
#include "huff_codes.h"

extern int decode_huffman_code_block_stateless(struct inflate_state *);

/* structure contain lookup data based on RFC 1951 */
struct rfc1951_tables {
	uint8_t dist_extra_bit_count[32];
	uint32_t dist_start[32];
	uint8_t len_extra_bit_count[32];
	uint16_t len_start[32];

};

/* The following tables are based on the tables in the deflate standard,
 * RFC 1951 page 11. */
static struct rfc1951_tables rfc_lookup_table = {
	.dist_extra_bit_count = {
				 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02,
				 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
				 0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x0a, 0x0a,
				 0x0b, 0x0b, 0x0c, 0x0c, 0x0d, 0x0d, 0x00, 0x00},

	.dist_start = {
		       0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0007, 0x0009, 0x000d,
		       0x0011, 0x0019, 0x0021, 0x0031, 0x0041, 0x0061, 0x0081, 0x00c1,
		       0x0101, 0x0181, 0x0201, 0x0301, 0x0401, 0x0601, 0x0801, 0x0c01,
		       0x1001, 0x1801, 0x2001, 0x3001, 0x4001, 0x6001, 0x0000, 0x0000},

	.len_extra_bit_count = {
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
				0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04,
				0x05, 0x05, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00},

	.len_start = {
		      0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000a,
		      0x000b, 0x000d, 0x000f, 0x0011, 0x0013, 0x0017, 0x001b, 0x001f,
		      0x0023, 0x002b, 0x0033, 0x003b, 0x0043, 0x0053, 0x0063, 0x0073,
		      0x0083, 0x00a3, 0x00c3, 0x00e3, 0x0102, 0x0000, 0x0000, 0x0000}
};


struct slver {
       uint16_t snum;
       uint8_t ver;
       uint8_t core;
};

/* Version info */
struct slver isal_inflate_init_slver_00010088;
struct slver isal_inflate_init_slver = { 0x0088, 0x01, 0x00 };
struct slver isal_inflate_stateless_slver_00010089;
struct slver isal_inflate_stateless_slver = { 0x0089, 0x01, 0x00 };
struct slver isal_inflate_slver_0001008a;
struct slver isal_inflate_slver = { 0x008a, 0x01, 0x00 };


/*Performs a copy of length repeat_length data starting at dest -
 * lookback_distance into dest. This copy copies data previously copied when the
 * src buffer and the dest buffer overlap. */
void inline byte_copy(uint8_t * dest, uint64_t lookback_distance, int repeat_length)
{
	uint8_t *src = dest - lookback_distance;

	for (; repeat_length > 0; repeat_length--)
		*dest++ = *src++;
}

/*
 * Returns integer with first length bits reversed and all higher bits zeroed
 */
uint16_t inline bit_reverse2(uint16_t bits, uint8_t length)
{
	bits = ((bits >> 1) & 0x55555555) | ((bits & 0x55555555) << 1);	// swap bits
	bits = ((bits >> 2) & 0x33333333) | ((bits & 0x33333333) << 2);	// swap pairs
	bits = ((bits >> 4) & 0x0F0F0F0F) | ((bits & 0x0F0F0F0F) << 4);	// swap nibbles
	bits = ((bits >> 8) & 0x00FF00FF) | ((bits & 0x00FF00FF) << 8);	// swap bytes
	return bits >> (16 - length);
}

/* Load data from the in_stream into a buffer to allow for handling unaligned data*/
void inline inflate_in_load(struct inflate_state *state, int min_required)
{
	uint64_t temp = 0;
	uint8_t new_bytes;

	if (state->read_in_length >= 64)
		return;

	if (state->avail_in >= 8) {
		/* If there is enough space to load a 64 bits, load the data and use
		 * that to fill read_in */
		new_bytes = 8 - (state->read_in_length + 7) / 8;
		temp = *(uint64_t *) state->next_in;

		state->read_in |= temp << state->read_in_length;
		state->next_in += new_bytes;
		state->avail_in -= new_bytes;
		state->read_in_length += new_bytes * 8;

	} else {
		/* Else fill the read_in buffer 1 byte at a time */
		while (state->read_in_length < 57 && state->avail_in > 0) {
			temp = *state->next_in;
			state->read_in |= temp << state->read_in_length;
			state->next_in++;
			state->avail_in--;
			state->read_in_length += 8;

		}
	}
}

/* Returns the next bit_count bits from the in stream and shifts the stream over
 * by bit-count bits */
uint64_t inflate_in_read_bits(struct inflate_state *state, uint8_t bit_count);
uint64_t inline inflate_in_read_bits(struct inflate_state *state, uint8_t bit_count)
{
	uint64_t ret;
	assert(bit_count < 57);

	/* Load inflate_in if not enough data is in the read_in buffer */
	if (state->read_in_length < bit_count)
		inflate_in_load(state, bit_count);

	ret = (state->read_in) & ((1 << bit_count) - 1);
	state->read_in >>= bit_count;
	state->read_in_length -= bit_count;

	return ret;
}

/* Sets result to the inflate_huff_code corresponding to the huffcode defined by
 * the lengths in huff_code_table,where count is a histogram of the appearance
 * of each code length */
void inline make_inflate_huff_code_large(struct inflate_huff_code_large *result,
					 struct huff_code *huff_code_table, int table_length,
					 uint16_t * count)
{
	int i, j, k;
	uint16_t code = 0;
	uint16_t next_code[MAX_HUFF_TREE_DEPTH + 1];
	uint16_t long_code_list[LIT_LEN];
	uint32_t long_code_length = 0;
	uint16_t temp_code_list[1 << (15 - DECODE_LOOKUP_SIZE_LARGE)];
	uint32_t temp_code_length;
	uint32_t long_code_lookup_length = 0;
	uint32_t max_length;
	uint16_t first_bits;
	uint32_t code_length;
	uint16_t long_bits;
	uint16_t min_increment;
	uint32_t code_list[LIT_LEN];
	uint32_t code_list_len;
	uint32_t count_total[17];
	uint32_t insert_index;
	uint32_t last_length;
	uint32_t copy_size;
	uint16_t *short_code_lookup = result->short_code_lookup;

	count_total[0] = 0;
	count_total[1] = 0;
	for (i = 2; i < 17; i++)
		count_total[i] = count_total[i - 1] + count[i - 1];

	code_list_len = count_total[16];
	for (i = 0; i < table_length; i++) {
		code_length = huff_code_table[i].length;
		if (code_length > 0) {
			insert_index = count_total[code_length];
			code_list[insert_index] = i;
			count_total[code_length]++;
		}
	}

	next_code[0] = code;
	for (i = 1; i < MAX_HUFF_TREE_DEPTH + 1; i++)
		next_code[i] = (next_code[i - 1] + count[i - 1]) << 1;

	last_length = huff_code_table[code_list[0]].length;
	copy_size = (1 << last_length);

	for (k = 0; k < code_list_len; k++) {
		i = code_list[k];
		if (huff_code_table[i].length > DECODE_LOOKUP_SIZE_LARGE)
			break;

		while (huff_code_table[i].length > last_length) {
			memcpy(short_code_lookup + copy_size, short_code_lookup,
			       sizeof(*short_code_lookup) * copy_size);
			last_length++;
			copy_size <<= 1;
		}

		huff_code_table[i].code =
		    bit_reverse2(next_code[huff_code_table[i].length],
				 huff_code_table[i].length);

		next_code[huff_code_table[i].length] += 1;

		/* Set lookup table to return the current symbol concatenated
		 * with the code length when the first DECODE_LENGTH bits of the
		 * address are the same as the code for the current symbol. The
		 * first 9 bits are the code, bits 14:10 are the code length,
		 * bit 15 is a flag representing this is a symbol*/
		short_code_lookup[huff_code_table[i].code] =
		    i | (huff_code_table[i].length) << 9;

	}

	while (DECODE_LOOKUP_SIZE_LARGE > last_length) {
		memcpy(short_code_lookup + copy_size, short_code_lookup,
		       sizeof(*short_code_lookup) * copy_size);
		last_length++;
		copy_size <<= 1;
	}

	while (k < code_list_len) {
		i = code_list[k];
		huff_code_table[i].code =
		    bit_reverse2(next_code[huff_code_table[i].length],
				 huff_code_table[i].length);

		next_code[huff_code_table[i].length] += 1;

		/* Store the element in a list of elements with long codes. */
		long_code_list[long_code_length] = i;
		long_code_length++;
		k++;
	}

	for (i = 0; i < long_code_length; i++) {
		/*Set the look up table to point to a hint where the symbol can be found
		 * in the list of long codes and add the current symbol to the list of
		 * long codes. */
		if (huff_code_table[long_code_list[i]].code == 0xFFFF)
			continue;

		max_length = huff_code_table[long_code_list[i]].length;
		first_bits =
		    huff_code_table[long_code_list[i]].code
		    & ((1 << DECODE_LOOKUP_SIZE_LARGE) - 1);

		temp_code_list[0] = long_code_list[i];
		temp_code_length = 1;

		for (j = i + 1; j < long_code_length; j++) {
			if ((huff_code_table[long_code_list[j]].code &
			     ((1 << DECODE_LOOKUP_SIZE_LARGE) - 1)) == first_bits) {
				if (max_length < huff_code_table[long_code_list[j]].length)
					max_length = huff_code_table[long_code_list[j]].length;
				temp_code_list[temp_code_length] = long_code_list[j];
				temp_code_length++;
			}
		}

		for (j = 0; j < temp_code_length; j++) {
			code_length = huff_code_table[temp_code_list[j]].length;
			long_bits =
			    huff_code_table[temp_code_list[j]].code >>
			    DECODE_LOOKUP_SIZE_LARGE;
			min_increment = 1 << (code_length - DECODE_LOOKUP_SIZE_LARGE);
			for (; long_bits < (1 << (max_length - DECODE_LOOKUP_SIZE_LARGE));
			     long_bits += min_increment) {
				result->long_code_lookup[long_code_lookup_length + long_bits] =
				    temp_code_list[j] | (code_length << 9);
			}
			huff_code_table[temp_code_list[j]].code = 0xFFFF;
		}
		result->short_code_lookup[first_bits] =
		    long_code_lookup_length | (max_length << 9) | 0x8000;
		long_code_lookup_length += 1 << (max_length - DECODE_LOOKUP_SIZE_LARGE);

	}
}

void inline make_inflate_huff_code_small(struct inflate_huff_code_small *result,
					 struct huff_code *huff_code_table, int table_length,
					 uint16_t * count)
{
	int i, j, k;
	uint16_t code = 0;
	uint16_t next_code[MAX_HUFF_TREE_DEPTH + 1];
	uint16_t long_code_list[LIT_LEN];
	uint32_t long_code_length = 0;
	uint16_t temp_code_list[1 << (15 - DECODE_LOOKUP_SIZE_SMALL)];
	uint32_t temp_code_length;
	uint32_t long_code_lookup_length = 0;
	uint32_t max_length;
	uint16_t first_bits;
	uint32_t code_length;
	uint16_t long_bits;
	uint16_t min_increment;
	uint32_t code_list[LIT_LEN];
	uint32_t code_list_len;
	uint32_t count_total[17];
	uint32_t insert_index;
	uint32_t last_length;
	uint32_t copy_size;
	uint16_t *short_code_lookup = result->short_code_lookup;

	count_total[0] = 0;
	count_total[1] = 0;
	for (i = 2; i < 17; i++)
		count_total[i] = count_total[i - 1] + count[i - 1];

	code_list_len = count_total[16];
	for (i = 0; i < table_length; i++) {
		code_length = huff_code_table[i].length;
		if (code_length > 0) {
			insert_index = count_total[code_length];
			code_list[insert_index] = i;
			count_total[code_length]++;
		}
	}

	next_code[0] = code;
	for (i = 1; i < MAX_HUFF_TREE_DEPTH + 1; i++)
		next_code[i] = (next_code[i - 1] + count[i - 1]) << 1;

	last_length = huff_code_table[code_list[0]].length;
	copy_size = (1 << last_length);

	for (k = 0; k < code_list_len; k++) {
		i = code_list[k];
		if (huff_code_table[i].length > DECODE_LOOKUP_SIZE_SMALL)
			break;

		while (huff_code_table[i].length > last_length) {
			memcpy(short_code_lookup + copy_size, short_code_lookup,
			       sizeof(*short_code_lookup) * copy_size);
			last_length++;
			copy_size <<= 1;
		}

		huff_code_table[i].code =
		    bit_reverse2(next_code[huff_code_table[i].length],
				 huff_code_table[i].length);

		next_code[huff_code_table[i].length] += 1;

		/* Set lookup table to return the current symbol concatenated
		 * with the code length when the first DECODE_LENGTH bits of the
		 * address are the same as the code for the current symbol. The
		 * first 9 bits are the code, bits 14:10 are the code length,
		 * bit 15 is a flag representing this is a symbol*/
		short_code_lookup[huff_code_table[i].code] =
		    i | (huff_code_table[i].length) << 9;

	}

	while (DECODE_LOOKUP_SIZE_SMALL > last_length) {
		memcpy(short_code_lookup + copy_size, short_code_lookup,
		       sizeof(*short_code_lookup) * copy_size);
		last_length++;
		copy_size <<= 1;
	}

	while (k < code_list_len) {
		i = code_list[k];
		huff_code_table[i].code =
		    bit_reverse2(next_code[huff_code_table[i].length],
				 huff_code_table[i].length);

		next_code[huff_code_table[i].length] += 1;

		/* Store the element in a list of elements with long codes. */
		long_code_list[long_code_length] = i;
		long_code_length++;
		k++;
	}

	for (i = 0; i < long_code_length; i++) {
		/*Set the look up table to point to a hint where the symbol can be found
		 * in the list of long codes and add the current symbol to the list of
		 * long codes. */
		if (huff_code_table[long_code_list[i]].code == 0xFFFF)
			continue;

		max_length = huff_code_table[long_code_list[i]].length;
		first_bits =
		    huff_code_table[long_code_list[i]].code
		    & ((1 << DECODE_LOOKUP_SIZE_SMALL) - 1);

		temp_code_list[0] = long_code_list[i];
		temp_code_length = 1;

		for (j = i + 1; j < long_code_length; j++) {
			if ((huff_code_table[long_code_list[j]].code &
			     ((1 << DECODE_LOOKUP_SIZE_SMALL) - 1)) == first_bits) {
				if (max_length < huff_code_table[long_code_list[j]].length)
					max_length = huff_code_table[long_code_list[j]].length;
				temp_code_list[temp_code_length] = long_code_list[j];
				temp_code_length++;
			}
		}

		for (j = 0; j < temp_code_length; j++) {
			code_length = huff_code_table[temp_code_list[j]].length;
			long_bits =
			    huff_code_table[temp_code_list[j]].code >>
			    DECODE_LOOKUP_SIZE_SMALL;
			min_increment = 1 << (code_length - DECODE_LOOKUP_SIZE_SMALL);
			for (; long_bits < (1 << (max_length - DECODE_LOOKUP_SIZE_SMALL));
			     long_bits += min_increment) {
				result->long_code_lookup[long_code_lookup_length + long_bits] =
				    temp_code_list[j] | (code_length << 9);
			}
			huff_code_table[temp_code_list[j]].code = 0xFFFF;
		}
		result->short_code_lookup[first_bits] =
		    long_code_lookup_length | (max_length << 9) | 0x8000;
		long_code_lookup_length += 1 << (max_length - DECODE_LOOKUP_SIZE_SMALL);

	}
}

/* Sets the inflate_huff_codes in state to be the huffcodes corresponding to the
 * deflate static header */
int inline setup_static_header(struct inflate_state *state)
{
	/* This could be turned into a memcpy of this functions output for
	 * higher speed, but then DECODE_LOOKUP_SIZE couldn't be changed without
	 * regenerating the table. */

	int i;
	struct huff_code lit_code[LIT_LEN + 2];
	struct huff_code dist_code[DIST_LEN + 2];

	/* These tables are based on the static huffman tree described in RFC
	 * 1951 */
	uint16_t lit_count[16] = {
		0, 0, 0, 0, 0, 0, 0, 24, 152, 112, 0, 0, 0, 0, 0, 0
	};
	uint16_t dist_count[16] = {
		0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	/* These for loops set the code lengths for the static literal/length
	 * and distance codes defined in the deflate standard RFC 1951 */
	for (i = 0; i < 144; i++)
		lit_code[i].length = 8;

	for (i = 144; i < 256; i++)
		lit_code[i].length = 9;

	for (i = 256; i < 280; i++)
		lit_code[i].length = 7;

	for (i = 280; i < LIT_LEN + 2; i++)
		lit_code[i].length = 8;

	for (i = 0; i < DIST_LEN + 2; i++)
		dist_code[i].length = 5;

	make_inflate_huff_code_large(&state->lit_huff_code, lit_code, LIT_LEN + 2, lit_count);
	make_inflate_huff_code_small(&state->dist_huff_code, dist_code, DIST_LEN + 2,
				     dist_count);

	state->block_state = ISAL_BLOCK_CODED;

	return 0;
}

/* Decodes the next symbol symbol in in_buffer using the huff code defined by
 * huff_code */
uint16_t decode_next_large(struct inflate_state * state,
			   struct inflate_huff_code_large * huff_code);
uint16_t inline decode_next_large(struct inflate_state *state,
				  struct inflate_huff_code_large *huff_code)
{
	uint16_t next_bits;
	uint16_t next_sym;
	uint32_t bit_count;
	uint32_t bit_mask;

	if (state->read_in_length <= DEFLATE_CODE_MAX_LENGTH)
		inflate_in_load(state, 0);

	next_bits = state->read_in & ((1 << DECODE_LOOKUP_SIZE_LARGE) - 1);

	/* next_sym is a possible symbol decoded from next_bits. If bit 15 is 0,
	 * next_code is a symbol. Bits 9:0 represent the symbol, and bits 14:10
	 * represent the length of that symbols huffman code. If next_sym is not
	 * a symbol, it provides a hint of where the large symbols containin
	 * this code are located. Note the hint is at largest the location the
	 * first actual symbol in the long code list.*/
	next_sym = huff_code->short_code_lookup[next_bits];

	if (next_sym < 0x8000) {
		/* Return symbol found if next_code is a complete huffman code
		 * and shift in buffer over by the length of the next_code */
		bit_count = next_sym >> 9;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		return next_sym & 0x1FF;

	} else {
		/* If a symbol is not found, perform a linear search of the long code
		 * list starting from the hint in next_sym */
		bit_mask = (next_sym - 0x8000) >> 9;
		bit_mask = (1 << bit_mask) - 1;
		next_bits = state->read_in & bit_mask;
		next_sym =
		    huff_code->long_code_lookup[(next_sym & 0x1FF) +
						(next_bits >> DECODE_LOOKUP_SIZE_LARGE)];
		bit_count = next_sym >> 9;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;
		return next_sym & 0x1FF;

	}
}

uint16_t decode_next_small(struct inflate_state * state,
			   struct inflate_huff_code_small * huff_code);
uint16_t inline decode_next_small(struct inflate_state *state,
				  struct inflate_huff_code_small *huff_code)
{
	uint16_t next_bits;
	uint16_t next_sym;
	uint32_t bit_count;
	uint32_t bit_mask;

	if (state->read_in_length <= DEFLATE_CODE_MAX_LENGTH)
		inflate_in_load(state, 0);

	next_bits = state->read_in & ((1 << DECODE_LOOKUP_SIZE_SMALL) - 1);

	/* next_sym is a possible symbol decoded from next_bits. If bit 15 is 0,
	 * next_code is a symbol. Bits 9:0 represent the symbol, and bits 14:10
	 * represent the length of that symbols huffman code. If next_sym is not
	 * a symbol, it provides a hint of where the large symbols containin
	 * this code are located. Note the hint is at largest the location the
	 * first actual symbol in the long code list.*/
	next_sym = huff_code->short_code_lookup[next_bits];

	if (next_sym < 0x8000) {
		/* Return symbol found if next_code is a complete huffman code
		 * and shift in buffer over by the length of the next_code */
		bit_count = next_sym >> 9;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;

		return next_sym & 0x1FF;

	} else {
		/* If a symbol is not found, perform a linear search of the long code
		 * list starting from the hint in next_sym */
		bit_mask = (next_sym - 0x8000) >> 9;
		bit_mask = (1 << bit_mask) - 1;
		next_bits = state->read_in & bit_mask;
		next_sym =
		    huff_code->long_code_lookup[(next_sym & 0x1FF) +
						(next_bits >> DECODE_LOOKUP_SIZE_SMALL)];
		bit_count = next_sym >> 9;
		state->read_in >>= bit_count;
		state->read_in_length -= bit_count;
		return next_sym & 0x1FF;

	}
}

/* Reads data from the in_buffer and sets the huff code corresponding to that
 * data */
int inline setup_dynamic_header(struct inflate_state *state)
{
	int i, j;
	struct huff_code code_huff[CODE_LEN_CODES];
	struct huff_code lit_and_dist_huff[LIT_LEN + DIST_LEN];
	struct huff_code *previous = NULL, *current;
	struct inflate_huff_code_small inflate_code_huff;
	uint8_t hclen, hdist, hlit;
	uint16_t code_count[16], lit_count[16], dist_count[16];
	uint16_t *count;
	uint16_t symbol;

	/* This order is defined in RFC 1951 page 13 */
	const uint8_t code_length_code_order[CODE_LEN_CODES] = {
		0x10, 0x11, 0x12, 0x00, 0x08, 0x07, 0x09, 0x06,
		0x0a, 0x05, 0x0b, 0x04, 0x0c, 0x03, 0x0d, 0x02,
		0x0e, 0x01, 0x0f
	};

	memset(code_count, 0, sizeof(code_count));
	memset(lit_count, 0, sizeof(lit_count));
	memset(dist_count, 0, sizeof(dist_count));
	memset(code_huff, 0, sizeof(code_huff));
	memset(lit_and_dist_huff, 0, sizeof(lit_and_dist_huff));

	/* These variables are defined in the deflate standard, RFC 1951 */
	hlit = inflate_in_read_bits(state, 5);
	hdist = inflate_in_read_bits(state, 5);
	hclen = inflate_in_read_bits(state, 4);

	/* Create the code huffman code for decoding the lit/len and dist huffman codes */
	for (i = 0; i < hclen + 4; i++) {
		code_huff[code_length_code_order[i]].length = inflate_in_read_bits(state, 3);

		code_count[code_huff[code_length_code_order[i]].length] += 1;
	}

	if (state->read_in_length < 0)
		return END_OF_INPUT;

	make_inflate_huff_code_small(&inflate_code_huff, code_huff, CODE_LEN_CODES,
				     code_count);

	/* Decode the lit/len and dist huffman codes using the code huffman code */
	count = lit_count;
	current = lit_and_dist_huff;

	while (current < lit_and_dist_huff + LIT_LEN + hdist + 1) {
		/* If finished decoding the lit/len huffman code, start decoding
		 * the distance code these decodings are in the same loop
		 * because the len/lit and dist huffman codes are run length
		 * encoded together. */
		if (current == lit_and_dist_huff + 257 + hlit)
			current = lit_and_dist_huff + LIT_LEN;

		if (current == lit_and_dist_huff + LIT_LEN)
			count = dist_count;

		symbol = decode_next_small(state, &inflate_code_huff);

		if (state->read_in_length < 0)
			return END_OF_INPUT;

		if (symbol < 16) {
			/* If a length is found, update the current lit/len/dist
			 * to have length symbol */
			count[symbol]++;
			current->length = symbol;
			previous = current;
			current++;

		} else if (symbol == 16) {
			/* If a repeat length is found, update the next repeat
			 * length lit/len/dist elements to have the value of the
			 * repeated length */
			if (previous == NULL)	/* No elements available to be repeated */
				return INVALID_BLOCK_HEADER;

			i = 3 + inflate_in_read_bits(state, 2);
			for (j = 0; j < i; j++) {
				*current = *previous;
				count[current->length]++;
				previous = current;

				if (current == lit_and_dist_huff + 256 + hlit) {
					current = lit_and_dist_huff + LIT_LEN;
					count = dist_count;

				} else
					current++;
			}

		} else if (symbol == 17) {
			/* If a repeat zeroes if found, update then next
			 * repeated zeroes length lit/len/dist elements to have
			 * length 0. */
			i = 3 + inflate_in_read_bits(state, 3);

			for (j = 0; j < i; j++) {
				previous = current;

				if (current == lit_and_dist_huff + 256 + hlit) {
					current = lit_and_dist_huff + LIT_LEN;
					count = dist_count;

				} else
					current++;
			}

		} else if (symbol == 18) {
			/* If a repeat zeroes if found, update then next
			 * repeated zeroes length lit/len/dist elements to have
			 * length 0. */
			i = 11 + inflate_in_read_bits(state, 7);

			for (j = 0; j < i; j++) {
				previous = current;

				if (current == lit_and_dist_huff + 256 + hlit) {
					current = lit_and_dist_huff + LIT_LEN;
					count = dist_count;

				} else
					current++;
			}
		} else
			return INVALID_BLOCK_HEADER;

	}

	if (state->read_in_length < 0)
		return END_OF_INPUT;

	make_inflate_huff_code_large(&state->lit_huff_code, lit_and_dist_huff, LIT_LEN,
				     lit_count);
	make_inflate_huff_code_small(&state->dist_huff_code, &lit_and_dist_huff[LIT_LEN],
				     DIST_LEN, dist_count);

	state->block_state = ISAL_BLOCK_CODED;

	return 0;
}

/* Reads in the header pointed to by in_stream and sets up state to reflect that
 * header information*/
int read_header(struct inflate_state *state)
{
	uint8_t bytes;
	uint32_t btype;
	uint16_t len, nlen;
	int ret = 0;

	/* btype and bfinal are defined in RFC 1951, bfinal represents whether
	 * the current block is the end of block, and btype represents the
	 * encoding method on the current block. */

	state->bfinal = inflate_in_read_bits(state, 1);
	btype = inflate_in_read_bits(state, 2);

	if (state->read_in_length < 0)
		ret = END_OF_INPUT;

	else if (btype == 0) {
		inflate_in_load(state, 40);
		bytes = state->read_in_length / 8;

		if (bytes < 4)
			return END_OF_INPUT;

		state->read_in >>= state->read_in_length % 8;
		state->read_in_length = bytes * 8;

		len = state->read_in & 0xFFFF;
		state->read_in >>= 16;
		nlen = state->read_in & 0xFFFF;
		state->read_in >>= 16;
		state->read_in_length -= 32;

		bytes = state->read_in_length / 8;

		state->next_in -= bytes;
		state->avail_in += bytes;
		state->read_in = 0;
		state->read_in_length = 0;

		/* Check if len and nlen match */
		if (len != (~nlen & 0xffff))
			return INVALID_NON_COMPRESSED_BLOCK_LENGTH;

		state->type0_block_len = len;
		state->block_state = ISAL_BLOCK_TYPE0;

		ret = 0;

	} else if (btype == 1)
		ret = setup_static_header(state);

	else if (btype == 2)
		ret = setup_dynamic_header(state);

	else
		ret = INVALID_BLOCK_HEADER;

	return ret;
}

/* Reads in the header pointed to by in_stream and sets up state to reflect that
 * header information*/
int read_header_stateful(struct inflate_state *state)
{
	uint64_t read_in_start = state->read_in;
	int32_t read_in_length_start = state->read_in_length;
	uint8_t *next_in_start = state->next_in;
	uint32_t avail_in_start = state->avail_in;
	int block_state_start = state->block_state;
	int ret;
	int copy_size;
	int bytes_read;

	if (block_state_start == ISAL_BLOCK_HDR) {
		/* Setup so read_header decodes data in tmp_in_buffer */
		copy_size = ISAL_INFLATE_MAX_HDR_SIZE - state->tmp_in_size;
		if (copy_size > state->avail_in)
			copy_size = state->avail_in;

		memcpy(&state->tmp_in_buffer[state->tmp_in_size], state->next_in, copy_size);
		state->next_in = state->tmp_in_buffer;
		state->avail_in = state->tmp_in_size + copy_size;
	}

	ret = read_header(state);

	if (block_state_start == ISAL_BLOCK_HDR) {
		/* Setup so state is restored to a valid state */
		bytes_read = state->next_in - state->tmp_in_buffer - state->tmp_in_size;
		if (bytes_read < 0)
			bytes_read = 0;
		state->next_in = next_in_start + bytes_read;
		state->avail_in = avail_in_start - bytes_read;
	}

	if (ret == END_OF_INPUT) {
		/* Save off data so header can be decoded again with more data */
		state->read_in = read_in_start;
		state->read_in_length = read_in_length_start;
		memcpy(&state->tmp_in_buffer[state->tmp_in_size], next_in_start,
		       avail_in_start);
		state->tmp_in_size += avail_in_start;
		state->avail_in = 0;
		state->next_in = next_in_start + avail_in_start;
		state->block_state = ISAL_BLOCK_HDR;
	} else
		state->tmp_in_size = 0;

	return ret;

}

int inline decode_literal_block(struct inflate_state *state)
{
	uint32_t len = state->type0_block_len;
	/* If the block is uncompressed, perform a memcopy while
	 * updating state data */

	state->block_state = ISAL_BLOCK_NEW_HDR;

	if (state->avail_out < len) {
		len = state->avail_out;
		state->block_state = ISAL_BLOCK_TYPE0;
	}

	if (state->avail_in < len) {
		len = state->avail_in;
		state->block_state = ISAL_BLOCK_TYPE0;
	}

	memcpy(state->next_out, state->next_in, len);

	state->next_out += len;
	state->avail_out -= len;
	state->total_out += len;
	state->next_in += len;
	state->avail_in -= len;
	state->type0_block_len -= len;

	if (state->avail_in == 0 && state->block_state != ISAL_BLOCK_NEW_HDR)
		return END_OF_INPUT;

	if (state->avail_out == 0 && state->type0_block_len > 0)
		return OUT_BUFFER_OVERFLOW;

	return 0;

}

/* Decodes the next block if it was encoded using a huffman code */
int decode_huffman_code_block_stateless_base(struct inflate_state *state)
{
	uint16_t next_lit;
	uint8_t next_dist;
	uint32_t repeat_length;
	uint32_t look_back_dist;
	uint64_t read_in_tmp;
	int32_t read_in_length_tmp;
	uint8_t *next_in_tmp;
	uint32_t avail_in_tmp;

	state->copy_overflow_length = 0;
	state->copy_overflow_distance = 0;

	while (state->block_state == ISAL_BLOCK_CODED) {
		/* While not at the end of block, decode the next
		 * symbol */
		inflate_in_load(state, 0);

		read_in_tmp = state->read_in;
		read_in_length_tmp = state->read_in_length;
		next_in_tmp = state->next_in;
		avail_in_tmp = state->avail_in;

		next_lit = decode_next_large(state, &state->lit_huff_code);

		if (state->read_in_length < 0) {
			state->read_in = read_in_tmp;
			state->read_in_length = read_in_length_tmp;
			state->next_in = next_in_tmp;
			state->avail_in = avail_in_tmp;
			return END_OF_INPUT;
		}

		if (next_lit < 256) {
			/* If the next symbol is a literal,
			 * write out the symbol and update state
			 * data accordingly. */
			if (state->avail_out < 1) {
				state->read_in = read_in_tmp;
				state->read_in_length = read_in_length_tmp;
				state->next_in = next_in_tmp;
				state->avail_in = avail_in_tmp;
				return OUT_BUFFER_OVERFLOW;
			}

			*state->next_out = next_lit;
			state->next_out++;
			state->avail_out--;
			state->total_out++;

		} else if (next_lit == 256) {
			/* If the next symbol is the end of
			 * block, update the state data
			 * accordingly */
			state->block_state = ISAL_BLOCK_NEW_HDR;

		} else if (next_lit < 286) {
			/* Else if the next symbol is a repeat
			 * length, read in the length extra
			 * bits, the distance code, the distance
			 * extra bits. Then write out the
			 * corresponding data and update the
			 * state data accordingly*/
			repeat_length =
			    rfc_lookup_table.len_start[next_lit - 257] +
			    inflate_in_read_bits(state,
						 rfc_lookup_table.len_extra_bit_count[next_lit
										      - 257]);
			next_dist = decode_next_small(state, &state->dist_huff_code);

			look_back_dist = rfc_lookup_table.dist_start[next_dist] +
			    inflate_in_read_bits(state,
						 rfc_lookup_table.dist_extra_bit_count
						 [next_dist]);

			if (state->read_in_length < 0) {
				state->read_in = read_in_tmp;
				state->read_in_length = read_in_length_tmp;
				state->next_in = next_in_tmp;
				state->avail_in = avail_in_tmp;
				return END_OF_INPUT;
			}

			if (look_back_dist > state->total_out)
				return INVALID_LOOK_BACK_DISTANCE;

			if (state->avail_out < repeat_length) {
				state->copy_overflow_length = repeat_length - state->avail_out;
				state->copy_overflow_distance = look_back_dist;
				repeat_length = state->avail_out;
			}

			if (look_back_dist > repeat_length)
				memcpy(state->next_out,
				       state->next_out - look_back_dist, repeat_length);
			else
				byte_copy(state->next_out, look_back_dist, repeat_length);

			state->next_out += repeat_length;
			state->avail_out -= repeat_length;
			state->total_out += repeat_length;

			if (state->copy_overflow_length > 0)
				return OUT_BUFFER_OVERFLOW;
		} else
			/* Else the read in bits do not
			 * correspond to any valid symbol */
			return INVALID_SYMBOL;
	}
	return 0;
}

void isal_inflate_init(struct inflate_state *state)
{

	state->read_in = 0;
	state->read_in_length = 0;
	state->next_in = NULL;
	state->avail_in = 0;
	state->next_out = NULL;
	state->avail_out = 0;
	state->total_out = 0;
	state->block_state = ISAL_BLOCK_NEW_HDR;
	state->bfinal = 0;
	state->type0_block_len = 0;
	state->copy_overflow_length = 0;
	state->copy_overflow_distance = 0;
	state->tmp_in_size = 0;
	state->tmp_out_processed = 0;
	state->tmp_out_valid = 0;
}

int isal_inflate_stateless(struct inflate_state *state)
{
	uint32_t ret = 0;

	state->read_in = 0;
	state->read_in_length = 0;
	state->block_state = ISAL_BLOCK_NEW_HDR;
	state->bfinal = 0;
	state->total_out = 0;

	while (state->block_state != ISAL_BLOCK_INPUT_DONE) {
		if (state->block_state == ISAL_BLOCK_NEW_HDR) {
			ret = read_header(state);

			if (ret)
				break;
		}

		if (state->block_state == ISAL_BLOCK_TYPE0)
			ret = decode_literal_block(state);
		else
			ret = decode_huffman_code_block_stateless(state);

		if (ret)
			break;
		if (state->bfinal != 0 && state->block_state == ISAL_BLOCK_NEW_HDR)
			state->block_state = ISAL_BLOCK_INPUT_DONE;
	}

	/* Undo count stuff of bytes read into the read buffer */
	state->next_in -= state->read_in_length / 8;
	state->avail_in += state->read_in_length / 8;

	return ret;
}

int isal_inflate(struct inflate_state *state)
{

	uint8_t *next_out = state->next_out;
	uint32_t avail_out = state->avail_out;
	uint32_t copy_size = 0;
	int32_t shift_size = 0;
	int ret = 0;

	if (state->block_state != ISAL_BLOCK_FINISH) {
		/* If space in tmp_out buffer, decompress into the tmp_out_buffer */
		if (state->tmp_out_valid < 2 * ISAL_INFLATE_HIST_SIZE) {
			/* Setup to start decoding into temp buffer */
			state->next_out = &state->tmp_out_buffer[state->tmp_out_valid];
			state->avail_out =
			    sizeof(state->tmp_out_buffer) - ISAL_INFLATE_SLOP -
			    state->tmp_out_valid;

			if ((int32_t) state->avail_out < 0)
				state->avail_out = 0;

			/* Decode into internal buffer until exit */
			while (state->block_state != ISAL_BLOCK_INPUT_DONE) {
				if (state->block_state == ISAL_BLOCK_NEW_HDR
				    || state->block_state == ISAL_BLOCK_HDR) {
					ret = read_header_stateful(state);

					if (ret)
						break;
				}

				if (state->block_state == ISAL_BLOCK_TYPE0) {
					ret = decode_literal_block(state);
				} else
					ret = decode_huffman_code_block_stateless(state);

				if (ret)
					break;
				if (state->bfinal != 0
				    && state->block_state == ISAL_BLOCK_NEW_HDR)
					state->block_state = ISAL_BLOCK_INPUT_DONE;
			}

			/* Copy valid data from internal buffer into out_buffer */
			if (state->copy_overflow_length != 0) {
				byte_copy(state->next_out, state->copy_overflow_distance,
					  state->copy_overflow_length);
				state->tmp_out_valid += state->copy_overflow_length;
				state->next_out += state->copy_overflow_length;
				state->total_out += state->copy_overflow_length;
				state->copy_overflow_distance = 0;
				state->copy_overflow_length = 0;
			}

			state->tmp_out_valid = state->next_out - state->tmp_out_buffer;

			/* Setup state for decompressing into out_buffer */
			state->next_out = next_out;
			state->avail_out = avail_out;
		}

		/* Copy data from tmp_out buffer into out_buffer */
		copy_size = state->tmp_out_valid - state->tmp_out_processed;
		if (copy_size > avail_out)
			copy_size = avail_out;

		memcpy(state->next_out,
		       &state->tmp_out_buffer[state->tmp_out_processed], copy_size);

		state->tmp_out_processed += copy_size;
		state->avail_out -= copy_size;
		state->next_out += copy_size;

		if (ret == INVALID_LOOK_BACK_DISTANCE || ret == INVALID_BLOCK_HEADER
		    || ret == INVALID_NON_COMPRESSED_BLOCK_LENGTH) {
			/* Set total_out to not count data in tmp_out_buffer */
			state->total_out -= state->tmp_out_valid - state->tmp_out_processed;
			return ret;
		}

		/* If all data from tmp_out buffer has been processed, start
		 * decompressing into the out buffer */
		if (state->tmp_out_processed == state->tmp_out_valid) {
			while (state->block_state != ISAL_BLOCK_INPUT_DONE) {
				if (state->block_state == ISAL_BLOCK_NEW_HDR
				    || state->block_state == ISAL_BLOCK_HDR) {
					ret = read_header_stateful(state);
					if (ret)
						break;
				}

				if (state->block_state == ISAL_BLOCK_TYPE0)
					ret = decode_literal_block(state);
				else
					ret = decode_huffman_code_block_stateless(state);
				if (ret)
					break;
				if (state->bfinal != 0
				    && state->block_state == ISAL_BLOCK_NEW_HDR)
					state->block_state = ISAL_BLOCK_INPUT_DONE;
			}
		}

		if (state->block_state != ISAL_BLOCK_INPUT_DONE) {
			/* Save decompression history in tmp_out buffer */
			if (state->tmp_out_valid == state->tmp_out_processed
			    && avail_out - state->avail_out >= ISAL_INFLATE_HIST_SIZE) {
				memcpy(state->tmp_out_buffer,
				       state->next_out - ISAL_INFLATE_HIST_SIZE,
				       ISAL_INFLATE_HIST_SIZE);
				state->tmp_out_valid = ISAL_INFLATE_HIST_SIZE;
				state->tmp_out_processed = ISAL_INFLATE_HIST_SIZE;

			} else if (state->tmp_out_processed >= ISAL_INFLATE_HIST_SIZE) {
				shift_size = state->tmp_out_valid - ISAL_INFLATE_HIST_SIZE;
				if (shift_size > state->tmp_out_processed)
					shift_size = state->tmp_out_processed;

				memmove(state->tmp_out_buffer,
					&state->tmp_out_buffer[shift_size],
					state->tmp_out_valid - shift_size);
				state->tmp_out_valid -= shift_size;
				state->tmp_out_processed -= shift_size;

			}

			if (state->copy_overflow_length != 0) {
				byte_copy(&state->tmp_out_buffer[state->tmp_out_valid],
					  state->copy_overflow_distance,
					  state->copy_overflow_length);
				state->tmp_out_valid += state->copy_overflow_length;
				state->total_out += state->copy_overflow_length;
				state->copy_overflow_distance = 0;
				state->copy_overflow_length = 0;
			}

			if (ret == INVALID_LOOK_BACK_DISTANCE
			    || ret == INVALID_BLOCK_HEADER
			    || ret == INVALID_NON_COMPRESSED_BLOCK_LENGTH)
				return ret;

		} else if (state->tmp_out_valid == state->tmp_out_processed)
			state->block_state = ISAL_BLOCK_FINISH;
	}

	return DECOMPRESSION_FINISHED;
}