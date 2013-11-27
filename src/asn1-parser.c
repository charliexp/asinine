/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string.h>

#include "asinine/asn1.h"

// X.690 11/2008 item 8.1.2.4.1
#define IDENTIFIER_MULTIPART_TAG     (31)

#define IDENTIFIER_TYPE_MASK  (1<<5)
#define IDENTIFIER_TAG_MASK   ((1<<5)-1)
#define IDENTIFIER_MULTIPART_TAG_MASK ((1<<7)-1)

#define IDENTIFIER_CLASS(x) (((x) & (3<<6)) >> 6)

#define IDENTIFIER_IS_PRIMITIVE(x) (((x) & IDENTIFIER_TYPE_MASK) == 0)
#define IDENTIFIER_TAG_IS_MULTIPART(x) (((x) & IDENTIFIER_TAG_MASK) == \
	IDENTIFIER_MULTIPART_TAG)

// X.690 11/2008 item 8.1.3.5 (a)
#define CONTENT_LENGTH_LONG_MASK       (1<<7)
#define CONTENT_LENGTH_MASK            ((1<<7)-1)
// X.690 11/2008 item 8.1.3.5 (c)
#define CONTENT_LENGTH_LONG_RESERVED   ((1<<7)-1)

#define CONTENT_LENGTH_IS_LONG_FORM(x) ((x) & CONTENT_LENGTH_LONG_MASK)

#define NUM(x) (sizeof x / sizeof *(x))

static void
update_depth(asn1_parser_t *parser)
{
	// Check whether we're at the end of the parent token. If so, we ascend one
	// level and update the depth.
	while (parser->current == parser->parents[parser->depth] &&
		parser->depth > 1) {
		parser->depth--;
	}
}

asn1_err_t
asn1_parser_init(asn1_parser_t *parser, asn1_token_t *token,
	const uint8_t *data, size_t length)
{
	if (parser == NULL || token == NULL || data == NULL || length == 0)
	{
		return ASN1_ERROR_INVALID;
	}

	memset(parser, 0, sizeof *parser);

	parser->token = token;
	parser->current = data;

	parser->parents[0] = data + length;

	return ASN1_OK;
}

asn1_err_t
asn1_parser_ascend(asn1_parser_t *parser, size_t levels)
{
	if (levels >= parser->constraint) {
		return ASN1_ERROR_INVALID;
	}

	parser->constraint -= levels;
	return ASN1_OK;
}

asn1_err_t
asn1_parser_descend(asn1_parser_t *parser)
{
	if (parser->constraint >= NUM(parser->parents)) {
		return ASN1_ERROR_INVALID;
	}

	parser->constraint += 1;
	return ASN1_OK;
}

void
asn1_parser_skip_children(asn1_parser_t *parser)
{
	const asn1_token_t * const token = parser->token;

	if (!token->is_primitive) {
		parser->current = token->end;

		update_depth(parser);
	}
}

bool
asn1_parser_is_within(const asn1_parser_t *parser, const asn1_token_t *token)
{
	return parser->current < token->end;
}

const asn1_token_t*
asn1_parser_token(const asn1_parser_t *parser)
{
	return parser->token;
}

asn1_err_t
asn1_parser_next(asn1_parser_t *parser)
{
#define INC_CURRENT do { \
		parser->current++; \
		if (parser->current >= parent) { \
			return ASN1_ERROR_INVALID; \
		} \
	} while (0)

	const uint8_t * const parent = parser->parents[parser->depth];
	asn1_token_t *token = parser->token;

	if (parser->current == parent) {
		return ASN1_ERROR_EOF;
	} else if (parser->current > parent) {
		return ASN1_ERROR_INVALID;
	}

	if (parser->constraint > 0 &&
		parser->constraint != parser->depth) {
		return ASN1_ERROR_INVALID;
	}

	memset(token, 0, sizeof *token);

	token->class = IDENTIFIER_CLASS(*parser->current);
	token->is_primitive = IDENTIFIER_IS_PRIMITIVE(*parser->current);

	// Type (8.1.2)
	token->type = *parser->current & IDENTIFIER_TAG_MASK;
	INC_CURRENT;

	if (token->type == IDENTIFIER_MULTIPART_TAG) {
		size_t bits;

		// 8.1.2.4.2
		bits = 0;
		token->type = 0;

		do {
			token->type <<= 7;
			token->type |= *parser->current & IDENTIFIER_MULTIPART_TAG_MASK;
			INC_CURRENT;

			bits += 7;
			if (bits > sizeof token->type * 8) {
				return ASN1_ERROR_MEMORY;
			}
		} while (*parser->current & 0x80);
	}

	// Length (8.1.3)
	if (CONTENT_LENGTH_IS_LONG_FORM(*parser->current)) {
		size_t i, num_bytes;

		num_bytes = *parser->current & CONTENT_LENGTH_MASK;

		if (num_bytes == CONTENT_LENGTH_LONG_RESERVED) {
			return ASN1_ERROR_INVALID;
		} else if (num_bytes == 0) {
			// Indefinite form is not supported (X.690 11/2008 8.1.3.6)
			return ASN1_ERROR_INVALID;
		} else if (num_bytes > sizeof token->length) {
			// TODO: Write a test for this
			return ASN1_ERROR_UNSUPPORTED;
		}

		token->length = 0;
		for (i = 0; i < num_bytes; i++) {
			INC_CURRENT;
			token->length = (token->length << 8) | *parser->current;
		}
	} else {
		token->length = *parser->current & CONTENT_LENGTH_MASK;
	}

	// At this point, parser->current is not necessarily valid. For example,
	// NULL tokens will have a data pointer that might well point after the
	// actual data.

	parser->current++;
	token->data = parser->current;
	token->end  = parser->current + token->length;

	if (parser->depth == 0 && token->end != parent) {
		return ASN1_ERROR_INVALID;
	} else if (token->end > parent) {
		return ASN1_ERROR_INVALID;
	}

	if (token->is_primitive) {
		parser->current = token->end;
	} else {
		parser->depth++;

		if (parser->depth >= NUM(parser->parents)) {
			return ASN1_ERROR_INVALID;
		}

		parser->parents[parser->depth] = token->end;
	}

	update_depth(parser);

	return ASN1_OK;
#undef INC_CURRENT
}