/*
 *    Copyright (C) 2016-2020 Grok Image Compression Inc.
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
 *
 *    This source code incorporates work covered by the BSD 2-clause license.
 *    Please see the LICENSE file in the root directory for details.
 *
 */

#include "grk_includes.h"

namespace grk {

TileComponent::TileComponent() :resolutions(nullptr),
								numresolutions(0),
								resolutions_to_decompress(0),
						#ifdef DEBUG_LOSSLESS_T2
								round_trip_resolutions(nullptr),
						#endif
							   m_sa(nullptr),
							   whole_tile_decoding(true),
							   m_is_encoder(false),
							   buf(nullptr),
							   m_tccp(nullptr)
{}

TileComponent::~TileComponent(){
	release_mem();
	delete buf;
}
void TileComponent::release_mem(){
	if (resolutions) {
		for (uint32_t resno = 0; resno < numresolutions; ++resno) {
			auto res = resolutions + resno;
			for (uint32_t bandno = 0; bandno < 3; ++bandno) {
				auto band = res->bands + bandno;
				delete[] band->precincts;
				band->precincts = nullptr;
			}
		}
		delete[] resolutions;
		resolutions = nullptr;
	}
	delete m_sa;
	m_sa = nullptr;
}
/**
 * Initialize tile component in unreduced tile component coordinates
 * (tile component coordinates take sub-sampling into account).
 *
 */
bool TileComponent::init(bool isEncoder,
						bool whole_tile,
						grk_rect_u32 unreduced_tile_comp_dims,
						grk_rect_u32 unreduced_tile_comp_region_dims,
						uint8_t prec,
						CodingParams *cp,
						TileCodingParams *tcp,
						TileComponentCodingParams* tccp,
						grk_plugin_tile *current_plugin_tile){
	uint32_t state = grk_plugin_get_debug_state();
	m_is_encoder = isEncoder;
	whole_tile_decoding = whole_tile;
	m_tccp = tccp;

	// 1. initialize resolutions, bands and buffer
	numresolutions = m_tccp->numresolutions;
	if (numresolutions < cp->m_coding_params.m_dec.m_reduce) {
		resolutions_to_decompress = 1;
	} else {
		resolutions_to_decompress = numresolutions
				- cp->m_coding_params.m_dec.m_reduce;
	}
	resolutions = new Resolution[numresolutions];
	for (uint32_t resno = 0; resno < numresolutions; ++resno) {
		auto res = resolutions + resno;
		uint32_t levelno = numresolutions - 1 - resno;

		/* border for each resolution level (global) */
		auto dim = unreduced_tile_comp_dims;
		*((grk_rect_u32*)res) = dim.rectceildivpow2(levelno);

		/* p. 35, table A-23, ISO/IEC FDIS154444-1 : 2000 (18 august 2000) */
		uint32_t pdx = m_tccp->prcw[resno];
		uint32_t pdy = m_tccp->prch[resno];
		/* p. 64, B.6, ISO/IEC FDIS15444-1 : 2000 (18 august 2000)  */
		uint32_t tprc_x_start = uint_floordivpow2(res->x0, pdx) << pdx;
		uint32_t tprc_y_start = uint_floordivpow2(res->y0, pdy) << pdy;
		uint64_t temp = (uint64_t)ceildivpow2<uint32_t>(res->x1, pdx) << pdx;
		if (temp > UINT_MAX){
			GRK_ERROR("Resolution x1 value %u must be less than 2^32", temp);
			return false;
		}
		uint32_t br_prc_x_end = (uint32_t)temp;
		temp = (uint64_t)ceildivpow2<uint32_t>(res->y1, pdy) << pdy;
		if (temp > UINT_MAX){
			GRK_ERROR("Resolution y1 value %u must be less than 2^32", temp);
			return false;
		}
		uint32_t br_prc_y_end = (uint32_t)temp;
		res->pw =	(res->x0 == res->x1) ?	0 : ((br_prc_x_end - tprc_x_start) >> pdx);
		res->ph =	(res->y0 == res->y1) ?	0 : ((br_prc_y_end - tprc_y_start) >> pdy);
		res->numbands = (resno == 0) ? 1 : 3;
		for (uint32_t bandno = 0; bandno < res->numbands; ++bandno) {
			auto band = res->bands + bandno;
			auto tile_comp = unreduced_tile_comp_dims;
			if (resno == 0) {
				band->bandno = 0;
				*((grk_rect_u32*)band) =  tile_comp.rectceildivpow2(levelno);
			} else {
				band->bandno = (uint8_t)(bandno + 1);
				uint32_t x0b = band->bandno & 1;  					/* x0b = 1 if bandno = 1 or 3 */
				uint32_t y0b = (uint32_t) (band->bandno >> 1); 	/* y0b = 1 if bandno = 2 or 3 */

				uint64_t off_x = ((uint64_t) x0b << levelno);
				uint64_t off_y =  ((uint64_t) y0b << levelno);

				/* band border (global) */
				*((grk_rect_u32*)band) = grk_rect_u32(
						uint64_ceildivpow2(tile_comp.x0 - off_x, levelno + 1),
						uint64_ceildivpow2(tile_comp.y0 - off_y, levelno + 1),
						uint64_ceildivpow2(tile_comp.x1 - off_x, levelno + 1),
						uint64_ceildivpow2(tile_comp.y1 - off_y, levelno + 1));
			}
		}
	}
	create_buffer(isEncoder, unreduced_tile_comp_region_dims);

	// 2. initialize precincts and code blocks
	for (uint32_t resno = 0; resno < numresolutions; ++resno) {
		auto res = resolutions + resno;

		/* p. 35, table A-23, ISO/IEC FDIS154444-1 : 2000 (18 august 2000) */
		uint32_t pdx = m_tccp->prcw[resno];
		uint32_t pdy = m_tccp->prch[resno];
		/* p. 64, B.6, ISO/IEC FDIS15444-1 : 2000 (18 august 2000)  */
		uint32_t tprc_x_start = uint_floordivpow2(res->x0, pdx) << pdx;
		uint32_t tprc_y_start = uint_floordivpow2(res->y0, pdy) << pdy;
		uint64_t nb_precincts = (uint64_t)res->pw * res->ph;
		if (mult64_will_overflow(nb_precincts, sizeof(Precinct))) {
			GRK_ERROR(	"nb_precinct_size calculation would overflow ");
			return false;
		}
		uint32_t tlcbgxstart, tlcbgystart;
		uint32_t cbgwidthexpn, cbgheightexpn;
		if (resno == 0) {
			tlcbgxstart = tprc_x_start;
			tlcbgystart = tprc_y_start;
			cbgwidthexpn = pdx;
			cbgheightexpn = pdy;
		} else {
			tlcbgxstart = ceildivpow2<uint32_t>(tprc_x_start, 1);
			tlcbgystart = ceildivpow2<uint32_t>(tprc_y_start, 1);
			cbgwidthexpn = pdx - 1;
			cbgheightexpn = pdy - 1;
		}

		uint32_t cblkwidthexpn 	= std::min<uint32_t>(tccp->cblkw, cbgwidthexpn);
		uint32_t cblkheightexpn = std::min<uint32_t>(tccp->cblkh, cbgheightexpn);
		size_t nominalBlockSize = (1 << cblkwidthexpn) * (1 << cblkheightexpn);

		for (uint32_t bandno = 0; bandno < res->numbands; ++bandno) {
			auto band = res->bands + bandno;
			tccp->quant.setBandStepSizeAndBps(tcp,
											band,
											resno,
											(uint8_t)bandno,
											tccp,
											prec,
											m_is_encoder);

			band->precincts = new Precinct[nb_precincts];
			band->numPrecincts = nb_precincts;
			for (uint64_t precno = 0; precno < nb_precincts; ++precno) {
				auto current_precinct = band->precincts + precno;
				uint32_t cbgxstart = tlcbgxstart + (uint32_t)(precno % res->pw) * (1 << cbgwidthexpn);
				uint32_t cbgystart = tlcbgystart + (uint32_t)(precno / res->pw) * (1 << cbgheightexpn);
				auto cbg = grk_rect_u32(cbgxstart,
										cbgystart,
										cbgxstart + (1 << cbgwidthexpn),
										cbgystart + (1 << cbgheightexpn));

				*((grk_rect_u32*)current_precinct) = cbg.intersection(band);

				uint32_t tlcblkxstart 	= uint_floordivpow2(current_precinct->x0,cblkwidthexpn) << cblkwidthexpn;
				uint32_t tlcblkystart 	= uint_floordivpow2(current_precinct->y0,cblkheightexpn) << cblkheightexpn;
				uint32_t brcblkxend 	= ceildivpow2<uint32_t>(current_precinct->x1,cblkwidthexpn) << cblkwidthexpn;
				uint32_t brcblkyend 	= ceildivpow2<uint32_t>(current_precinct->y1,cblkheightexpn) << cblkheightexpn;
				current_precinct->cw 	= ((brcblkxend - tlcblkxstart)	>> cblkwidthexpn);
				current_precinct->ch 	= ((brcblkyend - tlcblkystart)	>> cblkheightexpn);

				uint64_t nb_code_blocks = (uint64_t) current_precinct->cw* current_precinct->ch;
				if (nb_code_blocks > 0) {
					if (isEncoder)
						current_precinct->enc = new CompressCodeblock[nb_code_blocks];
					else
						current_precinct->dec = new DecompressCodeblock[nb_code_blocks];
				    current_precinct->numCodeBlocks = nb_code_blocks;
				}
				current_precinct->initTagTrees();

				for (uint64_t cblkno = 0; cblkno < nb_code_blocks; ++cblkno) {
					uint32_t cblkxstart = tlcblkxstart	+ (uint32_t) (cblkno % current_precinct->cw)* (1 << cblkwidthexpn);
					uint32_t cblkystart = tlcblkystart	+ (uint32_t) (cblkno / current_precinct->cw)* (1 << cblkheightexpn);
					auto cblk_bounds = grk_rect_u32(cblkxstart,
													cblkystart,
													cblkxstart + (1 << cblkwidthexpn),
													cblkystart + (1 << cblkheightexpn));

					auto cblk_dims = (m_is_encoder) ?
												(grk_rect_u32*)(current_precinct->enc + cblkno) :
												(grk_rect_u32*)(current_precinct->dec + cblkno);
					if (m_is_encoder) {
						auto code_block = current_precinct->enc + cblkno;
						if (!code_block->alloc())
							return false;
						if (!current_plugin_tile
								|| (state & GRK_PLUGIN_STATE_DEBUG)) {
							if (!code_block->alloc_data(
									nominalBlockSize))
								return false;
						}
					} else {
						auto code_block =
								current_precinct->dec + cblkno;
						if (!current_plugin_tile
								|| (state & GRK_PLUGIN_STATE_DEBUG)) {
							if (!code_block->alloc())
								return false;
						}
					}
					*cblk_dims = cblk_bounds.intersection(current_precinct);
				}
			}
		}
	}

	return true;
}

bool TileComponent::subbandIntersectsAOI(uint32_t resno,
								uint32_t bandno,
								const grk_rect_u32 *aoi) const
{
	if (whole_tile_decoding)
		return true;

    /* Note: those values for filter_margin are in part the result of */
    /* experimentation. The value 2 for QMFBID=1 (5x3 filter) and */
    /* 4 for QMFBID=0 (9x7 filter) can be linked to the */
    /* maximum left/right extension given in tables F.2 and F.3 of the */
    /* standard.*/
    uint32_t filter_margin = (m_tccp->qmfbid == 1) ? 2 : 4;
    auto b = resolutions[resno].bands[bandno];
    b.grow(filter_margin,filter_margin);
    return b.intersection(aoi).is_non_degenerate();
}

void TileComponent::allocSparseBuffer(uint32_t numres){
    auto tr_max = resolutions + numres - 1;
	uint32_t w = tr_max->width();
	uint32_t h = tr_max->height();
	auto sa = new SparseBuffer<6,6>(w, h);

    for (uint32_t resno = 0; resno < numres; ++resno) {
        auto res = &resolutions[resno];
        for (uint32_t bandno = 0; bandno < res->numbands; ++bandno) {
            auto band = res->bands + bandno;
            for (uint64_t precno = 0; precno < (uint64_t)res->pw * res->ph; ++precno) {
                auto precinct = band->precincts + precno;
                for (uint64_t cblkno = 0; cblkno < (uint64_t)precinct->cw * precinct->ch; ++cblkno) {
                    auto cblk = precinct->dec + cblkno;
					// check overlap in absolute coordinates
					if (subbandIntersectsAOI(resno,	bandno,	cblk)){
						uint32_t x = cblk->x0;
						uint32_t y = cblk->y0;

						// switch from coordinates relative to band,
						// to coordinates relative to current resolution
						x -= band->x0;
						y -= band->y0;

						/* add band offset relative to previous resolution */
						if (band->bandno & 1) {
							auto prev_res = resolutions + resno - 1;
							x += prev_res->x1 - prev_res->x0;
						}
						if (band->bandno & 2) {
							auto prev_res = resolutions + resno - 1;
							y += prev_res->y1 - prev_res->y0;
						}

						if (!sa->alloc(grk_rect_u32(x,
												  y,
												  x + cblk->width(),
												  y + cblk->height()))) {
							delete sa;
							throw runtime_error("unable to allocate sparse array");
						}
					}
                }
            }
        }
    }
    if (m_sa)
    	delete m_sa;
    m_sa = sa;
}

void TileComponent::create_buffer(bool isEncoder, grk_rect_u32 unreduced_tile_comp_region_dims) {
	auto highestNumberOfResolutions =
			(!m_is_encoder) ? resolutions_to_decompress : numresolutions;
	auto hightestResolution =  resolutions + highestNumberOfResolutions - 1;
	auto maxResolution = resolutions + numresolutions - 1;

	grk_rect_u32::operator=(*(grk_rect_u32*)hightestResolution);
	delete buf;
	buf = new TileComponentBuffer<int32_t>(isEncoder,
											grk_rect_u32(maxResolution->x0,
														maxResolution->y0,
														maxResolution->x1,
														maxResolution->y1),
											grk_rect_u32(x0,
														y0,
														x1,
														y1),
											unreduced_tile_comp_region_dims,
											resolutions,
											numresolutions,
											highestNumberOfResolutions,
											whole_tile_decoding);
}

TileComponentBuffer<int32_t>* TileComponent::getBuffer(){
	return buf;
}

bool TileComponent::isWholeTileDecoding() {
	return whole_tile_decoding;
}
ISparseBuffer* TileComponent::getSparseBuffer(){
	return m_sa;
}

}
