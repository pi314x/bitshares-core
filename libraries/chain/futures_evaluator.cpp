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
#include <graphene/chain/futures_evaluator.hpp>
#include <graphene/chain/futures_object.hpp>
#include <graphene/chain/oracle_object.hpp>

#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/hardfork.hpp>

namespace graphene { namespace chain {

void update_futures_mark_price( database& d, const futures_market_object& market )
{
   const oracle_object* o = d.find( market.oracle_id );

   optional<share_type> new_mark;
   if( nullptr != o && o->current_value.valid() )
   {
      // The oracle quotes base/quote as a ratio; a contract is priced as an integer amount of
      // the quote (== collateral) asset. Converting one contract's worth of the base through
      // the oracle price gives exactly that, using the same rounding asset::operator* has
      // always used rather than a second convention invented here.
      const asset one_contract( market.contract_size, o->base_asset );
      const asset in_collateral = one_contract * (*o->current_value);
      // A contract whose whole notional rounds to zero cannot be margined or liquidated
      // meaningfully, so it is treated as no mark at all rather than as a price of nothing.
      if( in_collateral.amount > 0 )
         new_mark = in_collateral.amount;
   }

   const auto now = d.head_block_time();
   if( market.mark_price == new_mark && market.mark_price_time == now )
      return;

   d.modify( market, [&new_mark, now]( futures_market_object& m ) {
      m.mark_price = new_mark;
      m.mark_price_time = now;
   } );
}

void update_futures_markets_for_oracle( database& d, const oracle_object& o )
{
   // Walks only the markets bound to this oracle, via the by_oracle index. Publishing happens
   // continuously, so it must never do work proportional to the total number of markets.
   const auto& idx = d.get_index_type<futures_market_index>().indices().get<by_oracle>();
   auto itr = idx.lower_bound( o.get_id() );
   const auto end = idx.upper_bound( o.get_id() );
   while( itr != end )
   {
      const futures_market_object& market = *itr;
      ++itr;   // update_futures_mark_price modifies the object, so advance first
      update_futures_mark_price( d, market );
   }
}

void_result futures_market_create_evaluator::do_evaluate(
      const futures_market_create_operation& op ) const
{ try {
   const database& d = db();
   const auto now = d.head_block_time();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( now ), "Not allowed until the futures hardfork" );

   const oracle_object& o = op.oracle_id(d);
   op.collateral_asset(d);   // throws if the asset does not exist

   // Margin, PnL and the mark price are all denominated in the collateral asset. Requiring the
   // oracle to quote against that same asset is what removes every unit conversion from
   // position accounting -- and a conversion is a division, which is a rounding decision.
   FC_ASSERT( o.quote_asset == op.collateral_asset,
              "Oracle '${n}' quotes against asset ${q}, but this market margins in ${c}; the "
              "oracle's quote asset must be the collateral asset",
              ("n", o.name)("q", o.quote_asset)("c", op.collateral_asset) );

   FC_ASSERT( o.base_asset != op.collateral_asset,
              "An oracle quoting an asset against itself cannot price a contract" );

   if( op.expiry.valid() )
   {
      FC_ASSERT( *op.expiry > now, "Expiry must be in the future" );
      FC_ASSERT( *op.expiry - now <= fc::days( GRAPHENE_FUTURES_MAX_EXPIRY_DAYS ),
                 "Expiry may not be more than ${d} days out",
                 ("d", GRAPHENE_FUTURES_MAX_EXPIRY_DAYS) );
   }

   const auto& idx = d.get_index_type<futures_market_index>().indices().get<by_futures_symbol>();
   FC_ASSERT( idx.find( op.symbol ) == idx.end(),
              "A futures market with symbol '${s}' already exists", ("s", op.symbol) );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

object_id_type futures_market_create_evaluator::do_apply(
      const futures_market_create_operation& op ) const
{ try {
   database& d = db();
   const auto now = d.head_block_time();

   const auto& market = d.create<futures_market_object>( [&op, now]( futures_market_object& m ) {
      m.owner             = op.owner;
      m.symbol            = op.symbol;
      m.description       = op.description;
      m.oracle_id         = op.oracle_id;
      m.collateral_asset  = op.collateral_asset;
      m.contract_size     = op.contract_size;
      m.expiry            = op.expiry;
      m.options           = op.options;
      m.last_funding_time = now;
   } );

   // Pick up whatever the oracle currently says, so a market is not born untradable purely
   // because nobody has published since it was created.
   update_futures_mark_price( d, market );

   return market.id;
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_market_update_evaluator::do_evaluate(
      const futures_market_update_operation& op )
{ try {
   const database& d = db();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( d.head_block_time() ),
              "Not allowed until the futures hardfork" );

   _market = &op.market_id(d);
   FC_ASSERT( _market->owner == op.owner, "Only the market's owner may update it" );
   FC_ASSERT( !_market->is_settled, "A settled market cannot be updated" );

   if( op.new_options.valid() && _market->open_interest > 0 )
   {
      // Raising the maintenance requirement under open positions would liquidate people who
      // were within the rules when they opened. Loosening it is allowed: it can only ever make
      // an existing position safer.
      FC_ASSERT( op.new_options->maintenance_margin_ratio
                 <= _market->options.maintenance_margin_ratio,
                 "Maintenance margin ratio cannot be raised while ${oi} contracts are open: "
                 "positions opened under the old rule would become liquidatable",
                 ("oi", _market->open_interest) );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_market_update_evaluator::do_apply(
      const futures_market_update_operation& op ) const
{ try {
   db().modify( *_market, [&op]( futures_market_object& m ) {
      if( op.new_description.valid() )
         m.description = *op.new_description;
      if( op.new_options.valid() )
         m.options = *op.new_options;
   } );
   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

} } // graphene::chain
