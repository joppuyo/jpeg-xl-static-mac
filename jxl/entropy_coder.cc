// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jxl/entropy_coder.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "jxl/ac_context.h"
#include "jxl/ac_strategy.h"
#include "jxl/aux_out.h"
#include "jxl/base/bits.h"
#include "jxl/base/compiler_specific.h"
#include "jxl/base/profiler.h"
#include "jxl/base/status.h"
#include "jxl/coeff_order.h"
#include "jxl/coeff_order_fwd.h"
#include "jxl/common.h"
#include "jxl/dec_ans.h"
#include "jxl/dec_bit_reader.h"
#include "jxl/dec_context_map.h"
#include "jxl/enc_context_map.h"
#include "jxl/epf.h"
#include "jxl/image.h"
#include "jxl/image_ops.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "jxl/entropy_coder.cc"
#include <hwy/foreach_target.h>

// SIMD code
#include <hwy/before_namespace-inl.h>
namespace jxl {
#include <hwy/begin_target-inl.h>

// Returns number of non-zero coefficients (but skip LLF).
// We cannot rely on block[] being all-zero bits, so first truncate to integer.
// Also writes the per-8x8 block nzeros starting at nzeros_pos.
int32_t NumNonZeroExceptLLF(const size_t cx, const size_t cy,
                            const AcStrategy acs, const size_t covered_blocks,
                            const size_t log2_covered_blocks,
                            const ac_qcoeff_t* JXL_RESTRICT block,
                            const size_t nzeros_stride,
                            int32_t* JXL_RESTRICT nzeros_pos) {
  const HWY_CAPPED(float, kBlockDim) df;
  const HWY_CAPPED(int32_t, kBlockDim) di;

  const auto zero = Zero(di);
  // Add FF..FF for every zero coefficient, negate to get #zeros.
  auto neg_sum_zero = zero;

  {
    // Mask sufficient for one row of coefficients.
    HWY_ALIGN const int32_t
        llf_mask_lanes[AcStrategy::kMaxCoeffBlocks * (1 + kBlockDim)] = {
            -1, -1, -1, -1};
    // First cx=1,2,4 elements are FF..FF, others 0.
    const int32_t* llf_mask_pos =
        llf_mask_lanes + AcStrategy::kMaxCoeffBlocks - cx;

    // Rows with LLF: mask out the LLF
    for (size_t y = 0; y < cy; y++) {
      for (size_t x = 0; x < cx * kBlockDim; x += Lanes(df)) {
        const auto llf_mask = BitCast(df, LoadU(di, llf_mask_pos + x));

        // LLF counts as zero so we don't include it in nzeros.
        const auto coef =
            AndNot(llf_mask, Load(df, &block[y * cx * kBlockDim + x]));

        neg_sum_zero += VecFromMask(ConvertTo(di, coef) == zero);
      }
    }
  }

  // Remaining rows: no mask
  for (size_t y = cy; y < cy * kBlockDim; y++) {
    for (size_t x = 0; x < cx * kBlockDim; x += Lanes(df)) {
      const auto coef = Load(df, &block[y * cx * kBlockDim + x]);
      neg_sum_zero += VecFromMask(ConvertTo(di, coef) == zero);
    }
  }

  // We want area - sum_zero, add because neg_sum_zero is already negated.
  const int32_t nzeros =
      int32_t(cx * cy * kDCTBlockSize) + GetLane(SumOfLanes(neg_sum_zero));

  const int32_t shifted_nzeros = static_cast<int32_t>(
      (nzeros + covered_blocks - 1) >> log2_covered_blocks);
  // Need non-canonicalized dimensions!
  for (size_t y = 0; y < acs.covered_blocks_y(); y++) {
    for (size_t x = 0; x < acs.covered_blocks_x(); x++) {
      nzeros_pos[x + y * nzeros_stride] = shifted_nzeros;
    }
  }

  return nzeros;
}

// Specialization for 8x8, where only top-left is LLF/DC.
// About 1% overall speedup vs. NumNonZeroExceptLLF.
int32_t NumNonZero8x8ExceptDC(const ac_qcoeff_t* JXL_RESTRICT block,
                              int32_t* JXL_RESTRICT nzeros_pos) {
  const HWY_CAPPED(float, kBlockDim) df;
  const HWY_CAPPED(int32_t, kBlockDim) di;

  const auto zero = Zero(di);
  // Add FF..FF for every zero coefficient, negate to get #zeros.
  auto neg_sum_zero = zero;

  {
    // First row has DC, so mask
    const size_t y = 0;
    HWY_ALIGN const int32_t dc_mask_lanes[kBlockDim] = {-1};

    for (size_t x = 0; x < kBlockDim; x += Lanes(df)) {
      const auto dc_mask = BitCast(df, Load(di, dc_mask_lanes + x));

      // DC counts as zero so we don't include it in nzeros.
      const auto coef = AndNot(dc_mask, Load(df, &block[y * kBlockDim + x]));

      neg_sum_zero += VecFromMask(ConvertTo(di, coef) == zero);
    }
  }

  // Remaining rows: no mask
  for (size_t y = 1; y < kBlockDim; y++) {
    for (size_t x = 0; x < kBlockDim; x += Lanes(df)) {
      const auto coef = Load(df, &block[y * kBlockDim + x]);
      neg_sum_zero += VecFromMask(ConvertTo(di, coef) == zero);
    }
  }

  // We want 64 - sum_zero, add because neg_sum_zero is already negated.
  const int32_t nzeros =
      int32_t(kDCTBlockSize) + GetLane(SumOfLanes(neg_sum_zero));

  *nzeros_pos = nzeros;

  return nzeros;
}

// The number of nonzeros of each block is predicted from the top and the left
// blocks, with opportune scaling to take into account the number of blocks of
// each strategy.  The predicted number of nonzeros divided by two is used as a
// context; if this number is above 63, a specific context is used.  If the
// number of nonzeros of a strategy is above 63, it is written directly using a
// fixed number of bits (that depends on the size of the strategy).
void TokenizeCoefficients(const coeff_order_t* JXL_RESTRICT orders,
                          const Rect& rect,
                          const ac_qcoeff_t* JXL_RESTRICT* JXL_RESTRICT ac_rows,
                          const AcStrategyImage& ac_strategy,
                          YCbCrChromaSubsampling cs,
                          Image3I* JXL_RESTRICT tmp_num_nzeroes,
                          std::vector<Token>* JXL_RESTRICT output,
                          const ImageB& qdc, const ImageI& qf,
                          const BlockCtxMap& block_ctx_map) {
  const size_t xsize_blocks = rect.xsize();
  const size_t ysize_blocks = rect.ysize();

  // TODO(user): update the estimate: usually less coefficients are used.
  output->reserve(output->size() +
                  3 * xsize_blocks * ysize_blocks * kDCTBlockSize);

  size_t offset[3] = {};
  const size_t nzeros_stride = tmp_num_nzeroes->PixelsPerRow();
  const size_t hshift = HShift(cs);
  const size_t vshift = VShift(cs);
  for (size_t by = 0; by < ysize_blocks; ++by) {
    size_t sbyc = by >> vshift;
    int32_t* JXL_RESTRICT row_nzeros[3] = {
        tmp_num_nzeroes->PlaneRow(0, sbyc),
        tmp_num_nzeroes->PlaneRow(1, by),
        tmp_num_nzeroes->PlaneRow(2, sbyc),
    };
    const int32_t* JXL_RESTRICT row_nzeros_top[3] = {
        sbyc == 0 ? nullptr : tmp_num_nzeroes->ConstPlaneRow(0, sbyc - 1),
        by == 0 ? nullptr : tmp_num_nzeroes->ConstPlaneRow(1, by - 1),
        sbyc == 0 ? nullptr : tmp_num_nzeroes->ConstPlaneRow(2, sbyc - 1),
    };
    const uint8_t* JXL_RESTRICT row_qdc =
        qdc.ConstRow(rect.y0() + by) + rect.x0();
    const int32_t* JXL_RESTRICT row_qf = rect.ConstRow(qf, by);
    AcStrategyRow acs_row = ac_strategy.ConstRow(rect, by);
    for (size_t bx = 0; bx < xsize_blocks; ++bx) {
      AcStrategy acs = acs_row[bx];
      if (!acs.IsFirstBlock()) continue;
      size_t sbxc = bx >> hshift;
      size_t cx = acs.covered_blocks_x();
      size_t cy = acs.covered_blocks_y();
      const size_t covered_blocks = cx * cy;  // = #LLF coefficients
      const size_t log2_covered_blocks =
          NumZeroBitsBelowLSBNonzero(covered_blocks);
      const size_t size = covered_blocks * kDCTBlockSize;

      CoefficientLayout(&cy, &cx);  // swap cx/cy to canonical order

      for (int c : {1, 0, 2}) {
        if (c != 1 && sbxc << hshift != bx) continue;
        if (c != 1 && sbyc << vshift != by) continue;
        size_t sbx = c == 1 ? bx : sbxc;
        const ac_qcoeff_t* JXL_RESTRICT block = ac_rows[c] + offset[c];

        int32_t nzeros =
            (covered_blocks == 1)
                ? NumNonZero8x8ExceptDC(block, row_nzeros[c] + sbx)
                : NumNonZeroExceptLLF(cx, cy, acs, covered_blocks,
                                      log2_covered_blocks, block, nzeros_stride,
                                      row_nzeros[c] + sbx);

        int ord = kStrategyOrder[acs.RawStrategy()];
        const coeff_order_t* JXL_RESTRICT order =
            &orders[CoeffOrderOffset(ord, c)];

        int32_t predicted_nzeros =
            PredictFromTopAndLeft(row_nzeros_top[c], row_nzeros[c], sbx, 32);
        size_t block_ctx =
            block_ctx_map.Context(row_qdc[bx], row_qf[sbx], ord, c);
        const int32_t nzero_ctx =
            block_ctx_map.NonZeroContext(predicted_nzeros, block_ctx);

        output->emplace_back(nzero_ctx, nzeros);
        const size_t histo_offset =
            block_ctx_map.ZeroDensityContextsOffset(block_ctx);
        // Skip LLF.
        size_t prev = (nzeros > size / 16 ? 0 : 1);
        for (size_t k = covered_blocks; k < size && nzeros != 0; ++k) {
          int32_t coeff = static_cast<int32_t>(block[order[k]]);
          size_t ctx =
              histo_offset + ZeroDensityContext(nzeros, k, covered_blocks,
                                                log2_covered_blocks, prev);
          uint32_t u_coeff = PackSigned(coeff);
          output->emplace_back(ctx, u_coeff);
          prev = coeff != 0;
          nzeros -= prev;
        }
        JXL_DASSERT(nzeros == 0);
        offset[c] += size;
      }
    }
  }
}

#include <hwy/end_target-inl.h>
}  // namespace jxl
#include <hwy/after_namespace-inl.h>

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(TokenizeCoefficients)
void TokenizeCoefficients(const coeff_order_t* JXL_RESTRICT orders,
                          const Rect& rect,
                          const ac_qcoeff_t* JXL_RESTRICT* JXL_RESTRICT ac_rows,
                          const AcStrategyImage& ac_strategy,
                          YCbCrChromaSubsampling cs,
                          Image3I* JXL_RESTRICT tmp_num_nzeroes,
                          std::vector<Token>* JXL_RESTRICT output,
                          const ImageB& qdc, const ImageI& qf,
                          const BlockCtxMap& block_ctx_map) {
  return HWY_DYNAMIC_DISPATCH(TokenizeCoefficients)(
      orders, rect, ac_rows, ac_strategy, cs, tmp_num_nzeroes, output, qdc, qf,
      block_ctx_map);
}

void EncodeBlockCtxMap(const BlockCtxMap& block_ctx_map, BitWriter* writer,
                       AuxOut* aux_out) {
  auto& dct = block_ctx_map.dc_thresholds;
  auto& qft = block_ctx_map.qf_thresholds;
  auto& ctx_map = block_ctx_map.ctx_map;
  BitWriter::Allotment allotment(
      writer,
      (dct[0].size() + dct[1].size() + dct[2].size() + qft.size()) * 34 + 1 +
          4 + 4 + ctx_map.size() * 10 + 1024);
  if (dct[0].empty() && dct[1].empty() && dct[2].empty() && qft.empty() &&
      ctx_map.size() == 21 &&
      std::equal(ctx_map.begin(), ctx_map.end(), BlockCtxMap::kDefaultCtxMap)) {
    writer->Write(1, 1);  // default
    ReclaimAndCharge(writer, &allotment, kLayerAC, aux_out);
    return;
  }
  writer->Write(1, 0);
  for (int j : {0, 1, 2}) {
    writer->Write(4, dct[j].size());
    for (int i : dct[j]) {
      JXL_CHECK(U32Coder::Write(kDCThresholdDist, PackSigned(i), writer));
    }
  }
  writer->Write(4, qft.size());
  for (int i : qft) {
    JXL_CHECK(U32Coder::Write(kQFThresholdDist, i - 1, writer));
  }
  EncodeContextMap(ctx_map, block_ctx_map.num_ctxs, allotment, writer);
  ReclaimAndCharge(writer, &allotment, kLayerAC, aux_out);
}

Status DecodeBlockCtxMap(BitReader* br, BlockCtxMap* block_ctx_map) {
  auto& dct = block_ctx_map->dc_thresholds;
  auto& qft = block_ctx_map->qf_thresholds;
  auto& ctx_map = block_ctx_map->ctx_map;
  bool is_default = br->ReadFixedBits<1>();
  if (is_default) {
    *block_ctx_map = BlockCtxMap();
    return true;
  }
  block_ctx_map->num_dc_ctxs = 1;
  for (int j : {0, 1, 2}) {
    dct[j].resize(br->ReadFixedBits<4>());
    block_ctx_map->num_dc_ctxs *= dct[j].size() + 1;
    for (int& i : dct[j]) {
      i = UnpackSigned(U32Coder::Read(kDCThresholdDist, br));
    }
  }
  qft.resize(br->ReadFixedBits<4>());
  for (int& i : qft) {
    i = U32Coder::Read(kQFThresholdDist, br) + 1;
  }

  if (block_ctx_map->num_dc_ctxs * (qft.size() + 1) > 64) {
    return JXL_FAILURE("Invalid block context map: too big");
  }

  ctx_map.resize(3 * BlockCtxMap::kNumStrategyOrders *
                 block_ctx_map->num_dc_ctxs * (qft.size() + 1));
  JXL_RETURN_IF_ERROR(DecodeContextMap(&ctx_map, &block_ctx_map->num_ctxs, br));
  if (block_ctx_map->num_ctxs > 16) {
    return JXL_FAILURE("Invalid block context map: too many distinct contexts");
  }
  return true;
}

constexpr uint8_t BlockCtxMap::kDefaultCtxMap[];

}  // namespace jxl
#endif  // HWY_ONCE
