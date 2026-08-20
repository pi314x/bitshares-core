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
#pragma once

#include <graphene/chain/types.hpp>
#include <graphene/protocol/futures.hpp>
#include <graphene/db/generic_index.hpp>

#include <boost/multi_index/composite_key.hpp>

namespace graphene { namespace chain {

using graphene::protocol::futures_market_options;

/**
 *  @brief A futures contract: perpetual or dated, margined and settled against an oracle.
 *  @ingroup object
 *  @ingroup protocol
 *
 *  Prices here -- @ref mark_price, @ref settlement_price, and order and position prices -- are
 *  integer amounts of @ref collateral_asset per contract, never ratios. See FUTURES-DESIGN.md:
 *  it is what makes position accounting exact, and in a leveraged market exactness is
 *  solvency.
 */
class futures_market_object : public abstract_object<futures_market_object, protocol_ids,
                                                     futures_market_object_type>
{
   public:
      account_id_type owner;
      string          symbol;          ///< Unique, e.g. "BTC-PERP"
      string          description;

      /// Index price source. Immutable: traders hold positions priced against it.
      oracle_id_type  oracle_id;
      /// Margin and PnL currency. Equal to the oracle's quote asset, so nothing is ever
      /// converted between units.
      asset_id_type   collateral_asset;
      /// Units of the oracle's base asset per contract. Immutable.
      share_type      contract_size;
      /// Absent means perpetual.
      optional<time_point_sec> expiry;

      futures_market_options options;

      // ---- state ----

      /**
       * Collateral per contract, derived from the oracle. Absent when the oracle has no value.
       *
       * Absent rather than zero or stale: risk is assessed against this, and a market that
       * kept liquidating against a dead price would be worse than one that pauses.
       */
      optional<share_type> mark_price;
      time_point_sec       mark_price_time;

      /// Contracts open on the long side. Equal to the short side by construction.
      share_type      open_interest;

      /// Monotone funding index, collateral per contract, signed. A position pays the
      /// difference between this and its own last value when next touched, so a funding tick
      /// does not have to walk every position.
      share_type      cumulative_funding;
      time_point_sec  last_funding_time;

      /// Holds liquidation penalties and rounding dust, and covers liquidation shortfalls.
      share_type      insurance_fund;

      /// Set once, at expiry.
      optional<share_type> settlement_price;
      bool            is_settled = false;

      bool is_perpetual()const { return !expiry.valid(); }

      /// @return whether new orders may be placed right now
      bool is_tradable( time_point_sec now )const
      {
         return options.enabled && !is_settled && mark_price.valid()
                && ( is_perpetual() || now < *expiry );
      }
};

struct by_futures_symbol;   ///< unique
struct by_owner;            ///< for API
struct by_oracle;           ///< to find the markets a published value must update
struct by_expiry;           ///< to find contracts due to settle

/**
* @ingroup object_index
*/
using futures_market_multi_index_type = multi_index_container<
   futures_market_object,
   indexed_by<
      ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
      ordered_unique< tag<by_futures_symbol>,
         member< futures_market_object, string, &futures_market_object::symbol >
      >,
      ordered_unique< tag<by_owner>,
         composite_key< futures_market_object,
            member< futures_market_object, account_id_type, &futures_market_object::owner >,
            member< object, object_id_type, &object::id >
         >
      >,
      ordered_unique< tag<by_oracle>,
         composite_key< futures_market_object,
            member< futures_market_object, oracle_id_type, &futures_market_object::oracle_id >,
            member< object, object_id_type, &object::id >
         >
      >
   >
>;

/**
* @ingroup object_index
*/
using futures_market_index = generic_index<futures_market_object,
                                           futures_market_multi_index_type>;

} } // graphene::chain

MAP_OBJECT_ID_TO_TYPE( graphene::chain::futures_market_object )

FC_REFLECT_TYPENAME( graphene::chain::futures_market_object )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::chain::futures_market_object )
