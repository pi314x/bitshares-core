/*
 * Copyright (c) 2026 BitShares contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <graphene/chain/oracle_object.hpp>

#include <fc/io/raw.hpp>
#include <fc/uint128.hpp>

#include <algorithm>

namespace graphene { namespace chain {

namespace {

constexpr uint64_t PPM_DENOM = 1000000;

/**
 * Exactly: does |candidate / reference - 1| exceed max_deviation_ppm?
 *
 * Both prices are ratios, so this is done by cross-multiplication in 128-bit integers rather
 * than by dividing. Nothing here may round: two nodes that rounded differently would compute
 * different aggregates from identical inputs and fork the chain.
 *
 *   candidate/reference = (cand.base * ref.quote) / (cand.quote * ref.base)
 *
 * and the test becomes
 *
 *   (1e6 - ppm) * cross_ref  <=  1e6 * cross_cand  <=  (1e6 + ppm) * cross_ref
 *
 * Amounts are bounded by GRAPHENE_MAX_SHARE_SUPPLY (1e15), so the widest product is
 * (1e15 * 1e15) * 2e6 = 2e36, comfortably inside uint128's ~3.4e38.
 */
bool deviates_too_far( const price& candidate, const price& reference, uint32_t max_deviation_ppm )
{
   const fc::uint128_t cross_cand = fc::uint128_t( candidate.base.amount.value )
                                  * reference.quote.amount.value;
   const fc::uint128_t cross_ref  = fc::uint128_t( candidate.quote.amount.value )
                                  * reference.base.amount.value;

   const fc::uint128_t mid  = cross_cand * PPM_DENOM;
   const fc::uint128_t low  = cross_ref  * ( PPM_DENOM - max_deviation_ppm );
   const fc::uint128_t high = cross_ref  * ( PPM_DENOM + max_deviation_ppm );

   return mid < low || mid > high;
}

/// The lowest value whose cumulative weight *exceeds* half the total.
///
/// Strictly greater, not greater-or-equal, so that with every weight equal to 1 this reduces
/// to the element at index size/2 -- exactly the median the legacy feed code picks
/// (asset_object.cpp, `effective_feeds.begin() + size/2`). That matters for migration: an
/// asset switching from legacy feeds to an oracle with the same producers must not see its
/// price jump because the two disagreed about which element of an even-sized set is "the"
/// median.
price weighted_median( vector<pair<price, uint32_t>>& entries )
{
   std::sort( entries.begin(), entries.end(),
              []( const pair<price, uint32_t>& a, const pair<price, uint32_t>& b )
              { return a.first < b.first; } );

   uint64_t total = 0;
   for( const auto& e : entries )
      total += e.second;

   uint64_t accumulated = 0;
   for( const auto& e : entries )
   {
      accumulated += e.second;
      if( accumulated * 2 > total )
         return e.first;
   }
   return entries.back().first; // unreachable while total > 0
}

/// Upper median for even-sized sets, matching both the weighted median above and the legacy
/// feed code. Deliberately not the mean of the two middle values: averaging means dividing,
/// and a rounded result is not something every node is guaranteed to agree on.
price upper_median( vector<price>& values )
{
   std::sort( values.begin(), values.end() );
   return values[ values.size() / 2 ];
}

} // namespace

void oracle_object::update_current_value( time_point_sec now )
{
   vector<pair<price, uint32_t>> live;
   live.reserve( submissions.size() );
   for( const auto& s : submissions )
   {
      if( !is_submission_live( s.second.first, now ) )
         continue;
      // A producer removed since publishing keeps its stored submission but stops counting,
      // so removing a misbehaving producer takes effect immediately rather than at expiry.
      const auto itr = options.producers.find( s.first );
      if( itr == options.producers.end() )
         continue;
      live.emplace_back( s.second.second, uint32_t( itr->second ) );
   }

   current_value_time = now;

   if( live.size() < size_t( options.minimum_producers ) )
   {
      // Reported as absent rather than stale. A consumer that kept using the last known value
      // through an outage would be trading on a number nobody is still asserting.
      current_value.reset();
      current_value_producer_count = 0;
      return;
   }

   if( options.max_deviation_ppm > 0 && current_value.valid() )
   {
      vector<pair<price, uint32_t>> kept;
      kept.reserve( live.size() );
      for( const auto& e : live )
      {
         if( !deviates_too_far( e.first, *current_value, options.max_deviation_ppm ) )
            kept.push_back( e );
      }
      // Only filter while enough non-outliers remain to satisfy quorum. If excluding them
      // would break quorum then every producer has moved together, which is what a real crash
      // looks like -- and freezing the oracle at exactly that moment is the failure mode this
      // rule exists to avoid.
      if( kept.size() >= size_t( options.minimum_producers ) )
         live = std::move( kept );
   }

   const price latest = weighted_median( live );
   current_value_producer_count = uint16_t( live.size() );

   history.emplace_back( now, latest );
   if( history.size() > size_t( GRAPHENE_ORACLE_MAX_HISTORY ) )
      history.erase( history.begin() );

   if( oracle_aggregation_method::median_of_latest == options.aggregation )
   {
      current_value = latest;
      return;
   }

   vector<price> in_window;
   in_window.reserve( history.size() );
   for( const auto& h : history )
   {
      if( ( now - h.first ).to_seconds() <= int64_t( options.window_sec ) )
         in_window.push_back( h.second );
   }
   // Never empty: the entry just appended carries timestamp `now`.
   current_value = upper_median( in_window );
}

} } // graphene::chain

FC_REFLECT_DERIVED_NO_TYPENAME( graphene::chain::oracle_object, (graphene::db::object),
                    (owner)
                    (name)
                    (description)
                    (base_asset)
                    (quote_asset)
                    (options)
                    (submissions)
                    (history)
                    (current_value)
                    (current_value_time)
                    (current_value_producer_count)
                  )

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::chain::oracle_object )
