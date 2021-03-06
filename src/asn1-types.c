/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string.h>

#include "asinine/asn1.h"

#define SECONDS_PER_YEAR  (31536000)
#define SECONDS_PER_MONTH  (2629744)
#define SECONDS_PER_DAY      (86400)
#define SECONDS_PER_HOUR      (3600)
#define SECONDS_PER_MINUTE      (60)

/** Y, M, D, H, "Z" */
#define MIN_DATA_LEN (4 * 2 + 1)

static bool
validate_string(const asn1_token_t *token)
{
	const uint8_t *data;

	if (token == NULL || token->class != ASN1_CLASS_UNIVERSAL) {
		return false;
	}

	switch (token->tag) {
	case ASN1_TYPE_PRINTABLESTRING:
		for (data = token->data; data < token->data + token->length; data++) {
			// Space
			if (*data == 0x20) {
				continue;
			}

			// ' and z
			if (*data < 0x27 || *data > 0x7a) {
				return false;
			}

			// Illegal characters: *, ;, <, >, @
			if (*data == 0x2a || *data == 0x3b || *data == 0x3c || *data == 0x3e
				|| *data == 0x40) {
				return false;
			}
		}
		break;

	case ASN1_TYPE_IA5STRING:
	case ASN1_TYPE_VISIBLESTRING:
	case ASN1_TYPE_T61STRING:
		for (data = token->data; data < token->data + token->length; data++) {
			/* Strictly speaking, control codes are allowed for IA5STRING, but
			 * since we don't have a way of dealing with code-page switching we
			 * restrict the type. This is non-conformant to the spec.
			 * Same goes for T61String, which can switch code pages mid-stream.
			 * We assume that the initial code-page is #6 (ASCII), and flag
			 * switching as an error. */
			if (*data < 0x20 || *data > 0x7f) {
				return false;
			}
		}
		break;

	case ASN1_TYPE_UTF8STRING: {
		enum {
			LEADING,
			CONTINUATION
		} state;
		int bytes;

		state = LEADING;
		bytes = 0;

		for (data = token->data; data < token->data + token->length; data++) {
			uint8_t byte = *data;

			switch (state) {
				case LEADING: {
					if (byte < 0x80) {
						continue;
					}

					if (0xC2 <= byte && byte < 0xD0) {
						bytes = 1;
					} else if (0xD0 <= byte && byte < 0xF5) {
						bytes = (byte >> 4) - 0xC;
					} else {
						// 0x80 - 0xBF: Continuation bytes
						// 0xC0 - 0xC1: Invalid code points
						return false;
					}

					state = CONTINUATION;
					break;
				}

				case CONTINUATION: {
					if (0x80 <= byte && byte < 0xC0) {
						bytes -= 1;

						if (bytes == 0) {
							state = LEADING;
						}

						continue;
					}

					return false;
				}
			}
		}
		break;
	}

	default:
		return false;
	}

	return true;
}

// 8.23
asinine_err_t
asn1_string(const asn1_token_t *token, char *buf, const size_t num)
{
	if (!validate_string(token)) {
		return ASININE_ERROR_INVALID;
	}

	if (num <= token->length) {
		return ASININE_ERROR_MEMORY;
	}

	memcpy(buf, token->data, token->length);
	buf[token->length] = '\0';

	// PRINTABLESTRING can not contain NULL characters per definition
	// TODO: Update me
	if (asn1_is(token, ASN1_CLASS_UNIVERSAL, ASN1_TYPE_IA5STRING) &&
		strlen(buf) != token->length) {
		return ASININE_ERROR_INVALID;
	}

	return ASININE_OK;
}

int
asn1_string_eq(const asn1_token_t *token, const char *str)
{
	if (!validate_string(token)) {
		return 0;
	}

	if (token->length != strlen(str)) {
		return 0;
	}

	return memcmp(token->data, str, token->length) == 0;
}

// 8.6
asinine_err_t
asn1_bitstring(const asn1_token_t *token, uint8_t *buf, const size_t num)
{
	// Thank you http://stackoverflow.com/a/2603254
	static const uint8_t lookup[16] = {
		0x0, 0x8, 0x4, 0xC,
		0x2, 0xA, 0x6, 0xE,
		0x1, 0x9, 0x5, 0xD,
		0x3, 0xB, 0x7, 0xF
	};

	/* First byte is number of unused bits in the last byte, must be <= 7. Last
	 * byte must not be 0, since it is not the smallest possible encoding.
	 * An empty bitstring is encoded as first byte 0 and no further data.
	 */
	uint8_t unused_bits;
	size_t i, j;

	// 8.6.2.2 and 10.2
	if (token->length < 1 || !token->is_primitive) {
		return ASININE_ERROR_INVALID;
	}

	if (token->length - 1 > num) {
		return ASININE_ERROR_MEMORY;
	}

	memset(buf, 0, num);
	unused_bits = token->data[0];

	// 8.6.2.2
	if (unused_bits > 7) {
		return ASININE_ERROR_INVALID;
	}

	// 8.6.2.3
	if (token->length == 1) {
		return (unused_bits == 0) ? ASININE_OK : ASININE_ERROR_INVALID;
	}

	// 11.2.2
	if (token->data[token->length - 1] == 0) {
		return ASININE_ERROR_INVALID;
	}

	// 11.2.1
	if (unused_bits > 0) {
		unused_bits = (1 << unused_bits) - 1;

		if ((token->data[token->length - 1] & unused_bits) != 0) {
			return ASININE_ERROR_INVALID;
		}
	}

	for (i = 1, j = 0; i < token->length; i++, j++) {
		const uint8_t data = token->data[i];

		buf[j] = (lookup[data & 0xf] << 4) | lookup[data >> 4];
	}

	return ASININE_OK;
}

// 8.3
asinine_err_t
asn1_int_unsafe(const asn1_token_t *token, int *value)
{
	bool negative;
	const uint8_t *data;

	if (token->length > sizeof *value) {
		return ASININE_ERROR_MEMORY;
	}

	data = token->data;
	if (*data & 0x80) {
		negative = true;
		*value = *data & 0x7F;
	} else {
		negative = false;
		*value = *data;
	}

	for (data += 1; data < token->data + token->length; data++) {
		*value = (*value << 8) | *data;
	}

	if (negative) {
		*value = *value * -1;
	}

	return ASININE_OK;
}

asinine_err_t
asn1_int(const asn1_token_t *token, int *value)
{
	// TODO: 8.3.2
	if (!asn1_is_int(token)) {
		return ASININE_ERROR_INVALID;
	}

	return asn1_int_unsafe(token, value);
}

static bool
decode_pair(const char *data, int *pair)
{
	if (data[0] < 0x30 || data[0] > 0x39 || data[1] < 0x30 || data[1] > 0x39) {
		return false;
	}

	*pair = (data[0] - 0x30) * 10 + (data[1] - 0x30);
	return true;
}

asinine_err_t
asn1_time(const asn1_token_t *token, asn1_time_t *time)
{
	// YYMMDDHHMM(SS)(Z|+-D)
	static const uint8_t days_per_month[12] = {
		// Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec
		    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	const char *data = (char *)token->data;

	union {
		struct part {
			int year;
			int month;
			int day;
			int hour;
			int minute;
			int second;
		} part;
		int raw[6];
	} t;
	struct part *part = &t.part;

	int i;
	int is_leap, leap_days;

	part->year = part->month = part->day = part->hour = part->minute =
		part->second = -1;

	if (token->length < MIN_DATA_LEN) {
		return ASININE_ERROR_INVALID;
	}

	for (i = 0; i < 5; data += 2, i++) {
		if (!decode_pair(data, &t.raw[i])) {
			return ASININE_ERROR_INVALID;
		}
	}

	if (*data != 'Z') {
		// Try to decode seconds
		if (data + 2 >= (char*)(token->data + token->length)) {
			// Need at least another char for seconds, plus 'Z' or timezone
			return ASININE_ERROR_INVALID;
		}

		if (!decode_pair(data, &part->second)) {
			return ASININE_ERROR_INVALID;
		}
		data += 2;
	}

	if (*data != 'Z') {
		// TODO: Parse timezone offset (which is not standards conformant)
		// TODO: If time did not include seconds, do we need to parse
		// non-conformant timezone offset?
		return ASININE_ERROR_INVALID;
	}

	// Validation
	if (token->tag == ASN1_TYPE_UTCTIME) {
		// Years are from (19)50 to (20)49, so 99 is 1999 and 00 is 2000.
		if (part->year < 0 || part->year > 99) {
			return ASININE_ERROR_INVALID;
		}

		// Normalize years, since the encoding is not linear:
		// 00 -> 2000, 49 -> 2049, 50 -> 1950, 99 -> 1999
		part->year += (part->year > 49) ? 1900 : 2000;
	} else {
		return ASININE_ERROR_INVALID;
	}

	is_leap = part->year % 4 == 0 &&
		(part->year % 100 != 0 || part->year % 400 == 0);

	if (part->month < 1 || part->month > 12) {
		return ASININE_ERROR_INVALID;
	}

	if (part->day < 1) {
		return ASININE_ERROR_INVALID;
	} else if (is_leap && part->month == 2) {
		// Check February in leap years
		if (part->day > 29) {
			return ASININE_ERROR_INVALID;
		}
	} else if (part->day > days_per_month[part->month - 1]) {
		return ASININE_ERROR_INVALID;
	}

	if (part->hour < 0 || part->hour > 23) {
		return ASININE_ERROR_INVALID;
	}

	// Seconds are "optional"
	part->second = (part->second == -1) ? 0 : part->second;
	if (part->second < 0 || part->second > 59) {
		return ASININE_ERROR_INVALID;
	}

	// Convert to UNIX time (approximately)
	leap_days = (part->year - 1968) / 4 - (part->year - 1900) / 100 +
		(part->year - 1600) / 400;

	if (is_leap && part->month < 3) {
		// Do not add leap day if current year is leap year and date specified
		// is before March 1st
		leap_days -= 1;
	}

	part->year  -= 1970;
	part->month -= 1;
	part->day   -= 1;

	*time = part->year * SECONDS_PER_YEAR;

	for (i = 0; i < part->month; i++) {
		*time += days_per_month[i] * SECONDS_PER_DAY;
	}

	*time += part->day  * SECONDS_PER_DAY;
	*time += part->hour * SECONDS_PER_HOUR;
	*time += part->minute * SECONDS_PER_MINUTE;
	*time += part->second;

	*time += leap_days * SECONDS_PER_DAY;

	return ASININE_OK;
}

asinine_err_t
asn1_bool_unsafe(const asn1_token_t *token, bool *value)
{
	uint8_t data;

	if (token->length != 1) {
		return ASININE_ERROR_INVALID;
	}

	data = *token->data;
	// 11.1
	if (data == 0x00) {
		*value = false;
	} else if (data == 0xFF) {
		*value = true;
	} else {
		return ASININE_ERROR_INVALID;
	}

	return ASININE_OK;
}

asinine_err_t
asn1_bool(const asn1_token_t *token, bool *value)
{
	if (!asn1_is_bool(token)) {
		return ASININE_ERROR_INVALID;
	}

	return asn1_bool_unsafe(token, value);
}

const char*
asn1_type_to_string(asn1_class_t class, asn1_tag_t type)
{
	if (class != ASN1_CLASS_UNIVERSAL) {
		return "INVALID CLASS";
	}

#define case_for_type(x) case x: return #x
	switch((asn1_universal_tag_t)type) {
		case_for_type(ASN1_TYPE_BOOL);
		case_for_type(ASN1_TYPE_INT);
		case_for_type(ASN1_TYPE_BITSTRING);
		case_for_type(ASN1_TYPE_OCTETSTRING);
		case_for_type(ASN1_TYPE_NULL);
		case_for_type(ASN1_TYPE_OID);
		case_for_type(ASN1_TYPE_UTF8STRING);
		case_for_type(ASN1_TYPE_SEQUENCE);
		case_for_type(ASN1_TYPE_SET);
		case_for_type(ASN1_TYPE_PRINTABLESTRING);
		case_for_type(ASN1_TYPE_T61STRING);
		case_for_type(ASN1_TYPE_IA5STRING);
		case_for_type(ASN1_TYPE_UTCTIME);
		case_for_type(ASN1_TYPE_GENERALIZEDTIME);
		case_for_type(ASN1_TYPE_VISIBLESTRING);
	}
#undef case_for_type

	return "UNKNOWN";
}

const uint8_t*
asn1_raw(const asn1_token_t *token)
{
	if (token->data == NULL || token->length == 0) {
		return NULL;
	}

	return token->data;
}

bool
asn1_eq(const asn1_token_t *a, const asn1_token_t *b)
{
	// TODO: Check that both tokens are ->is_valid?
	return (a->length == b->length) &&
	       (a->class == b->class) &&
	       (a->tag == b->tag) &&
	       (a->is_primitive == b->is_primitive) &&
	       (memcmp(a->data, b->data, a->length) == 0);
}

int
asn1_is(const asn1_token_t *token, asn1_class_t class, asn1_tag_t tag)
{
	return (token != NULL) && (token->class == class) &&
		(token->tag == tag);
}

int
asn1_is_time(const asn1_token_t *token)
{
	return (token != NULL) &&
		(token->class == ASN1_CLASS_UNIVERSAL) &&
		(token->tag == ASN1_TYPE_UTCTIME);
}

int
asn1_is_string(const asn1_token_t *token)
{
	return (token != NULL) &&
		(token->class == ASN1_CLASS_UNIVERSAL) &&
		(token->tag == ASN1_TYPE_PRINTABLESTRING ||
			token->tag == ASN1_TYPE_IA5STRING ||
			token->tag == ASN1_TYPE_UTF8STRING ||
			token->tag == ASN1_TYPE_VISIBLESTRING ||
			token->tag == ASN1_TYPE_T61STRING);
}
