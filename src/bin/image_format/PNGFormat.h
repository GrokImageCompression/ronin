/*
*    Copyright (C) 2016 Grok Image Compression Inc.
*
*    This source code is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This source code is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
 */
#pragma once

#include "ImageFormat.h"
#include <png.h>
#include <string>


void pngSetVerboseFlag(bool verbose);

class PNGFormat : public ImageFormat {
public:
	PNGFormat();
	bool encodeHeader(grk_image *  image, const std::string &filename, uint32_t compressionParam) override;
	bool encodeStrip(size_t rows) override;
	bool encodeFinish(void) override;
	grk_image *  decode(const std::string &filename,  grk_cparameters  *parameters) override;

private:
	int do_encode(grk_image *image, const char *write_idf,	uint32_t compressionLevel);
	grk_image* do_decode(const char *read_idf, grk_cparameters *params);

	png_infop info;
	png_structp png;
	std::string fileName;
	FILE *file;
	uint8_t *row_buf;
	uint8_t **row_buf_array;
	int32_t *row32s;
	grk_image *image;
	bool useStd;
	GRK_COLOR_SPACE colorSpace;
	bool fails;
};

