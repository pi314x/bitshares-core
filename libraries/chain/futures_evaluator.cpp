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

#include <fc/uint128.hpp>

#include <algorithm>

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

   // A new mark is the natural moment to check whether a funding interval has elapsed: it is
   // driven by the oracle, which publishes continuously, so no separate timer is needed.
   accrue_futures_funding( d, market );
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


namespace {

/**
 * Realises a position's PnL against the mark, moving it through the market's settlement pool.
 *
 * The pool is the piece that was missing. Deducting a loser's loss from their margin and
 * stopping there DESTROYS it -- the offsetting gain is still unrealised in some other
 * position, so nothing balances and the supply check shows collateral vanishing (it did: 80
 * units, when liquidation settled a position without a pool to settle into).
 *
 * With the pool, value moves rather than evaporating: losers pay into it, winners are paid out
 * of it, and margin + pool is conserved exactly. It can run negative when gains are realised
 * before the matching losses are collected, which is recovered as the losing positions are
 * next touched or liquidated.
 *
 * Exact, like everything else here: no division.
 */
/**
 * Charges a position whatever funding has accrued since it was last touched.
 *
 * Longs pay shorts when the book trades above the mark, and the reverse below it. The payment
 * is size x (index delta), so the two sides are exactly antisymmetric and it nets to zero
 * across the market -- it moves through the settlement pool for the same reason PnL does.
 */
void apply_funding( database& d, const futures_market_object& market,
                    const futures_position_object& pos )
{
   const share_type delta = market.cumulative_funding - pos.last_cumulative_funding;
   if( 0 == delta.value )
      return;

   // Positive index delta means longs pay: a long has size > 0, so payment > 0 leaves them.
   const share_type payment = pos.size * delta;
   d.modify( pos, [&]( futures_position_object& p ) {
      p.margin                  -= payment;
      p.last_cumulative_funding  = market.cumulative_funding;
   } );
   d.modify( market, [&payment]( futures_market_object& m ) { m.insurance_fund += payment; } );
}

void settle_to_mark( database& d, const futures_market_object& market,
                     const futures_position_object& pos )
{
   apply_funding( d, market, pos );

   if( !market.mark_price.valid() )
      return;
   const share_type mark = *market.mark_price;
   const share_type pnl  = pos.size * mark - pos.entry_value;
   if( 0 == pnl.value )
      return;

   d.modify( pos, [&]( futures_position_object& p ) {
      p.margin      += pnl;
      p.entry_value  = p.size * mark;
   } );
   d.modify( market, [&pnl]( futures_market_object& m ) { m.insurance_fund -= pnl; } );
}

/**
 * Applies one fill of @p n contracts at @p price to @p account's position in @p market.
 *
 * The whole of position accounting is these four lines, and that is the point. A fill adds
 * +n x price to a long's entry_value and -n x price to a short's, so Sum(entry_value) over the
 * market stays exactly zero, as does Sum(size). Nothing is realised on a partial close and
 * nothing is divided; a position that reaches zero holds exactly -entry_value of realised PnL,
 * which is paid out when it is removed.
 *
 * @param margin_in collateral moving from the order's reservation into the position
 * @return the change in open interest caused by this fill for this side
 */
share_type apply_fill( database& d, const futures_market_object& market, account_id_type account,
                       bool is_long, share_type n, share_type price, share_type margin_in )
{
   const share_type delta      = is_long ? n : -n;
   const share_type delta_value = delta * price;

   const auto& idx = d.get_index_type<futures_position_index>().indices().get<by_market_owner>();
   auto itr = idx.find( boost::make_tuple( market.get_id(), account ) );

   if( itr == idx.end() )
   {
      d.create<futures_position_object>( [&]( futures_position_object& p ) {
         p.owner                   = account;
         p.market_id               = market.get_id();
         p.size                    = delta;
         p.entry_value             = delta_value;
         p.margin                  = margin_in;
         p.last_cumulative_funding = market.cumulative_funding;

         // Settled to the mark below, once the object exists.
      } );
      settle_to_mark( d, market, *idx.find( boost::make_tuple( market.get_id(), account ) ) );
      return n;   // wholly opening
   }

   const futures_position_object& pos = *itr;
   const share_type old_abs = pos.abs_size();
   const share_type new_size = pos.size + delta;

   if( 0 == new_size.value )
   {
      // Flat. unrealized = 0 x mark - entry_value = -entry_value, exactly the realised PnL,
      // so the payout is margin - entry_value with no rounding anywhere.
      const share_type final_entry = pos.entry_value + delta_value;
      const share_type payout = pos.margin + margin_in - final_entry;

      // The payout can exceed what this trader ever deposited -- that is what a profit IS --
      // and the excess has to come from somewhere real. It comes from the settlement pool,
      // which the counterparty paid into when they opened away from the mark. Without this the
      // profit is conjured and the supply check shows collateral appearing (it did: 200 units).
      d.modify( market, [&final_entry]( futures_market_object& m )
                        { m.insurance_fund += final_entry; } );

      if( payout > 0 )
         d.adjust_balance( account, asset( payout, market.collateral_asset ) );
      d.remove( pos );
      return -old_abs;
   }

   d.modify( pos, [&]( futures_position_object& p ) {
      p.size        = new_size;
      p.entry_value = p.entry_value + delta_value;
      p.margin      = p.margin + margin_in;

   } );

   settle_to_mark( d, market, pos );

   const share_type new_abs = new_size >= 0 ? new_size : -new_size;
   return new_abs - old_abs;
}

/**
 * After a fill, the position must be able to stand at the CURRENT MARK, not merely at the
 * price it traded at.
 *
 * This is the check whose absence let a market pay out collateral it never held. A trade at a
 * price far from the mark opens a position that is already underwater by the difference: buy
 * at 120 while the mark is 100 and ten contracts are down 200 before the block ends. Margin
 * sized on the fill price does not cover that, and when the counterparty later closes at a
 * profit the market has to pay from money nobody posted.
 *
 * Requiring equity-at-mark to meet the initial margin requirement closes it: nobody can open a
 * position whose immediate mark-to-market loss exceeds what they put up. Failing here rolls
 * the whole operation back atomically.
 */
void require_margin_at_mark( database& d, const futures_market_object& market,
                             account_id_type account )
{
   const auto& idx = d.get_index_type<futures_position_index>().indices().get<by_market_owner>();
   auto itr = idx.find( boost::make_tuple( market.get_id(), account ) );
   if( itr == idx.end() )
      return;   // the fill closed the position outright

   FC_ASSERT( market.mark_price.valid(), "Market has no mark price" );
   const share_type mark = *market.mark_price;

   const share_type equity   = itr->equity( mark );
   const share_type required = futures_margin_required( itr->abs_size(), mark,
                                                        market.options.initial_margin_ratio );
   if( equity >= required )
      return;

   // Trading away from the mark is allowed, but it has to be funded. The order reserved margin
   // against its own limit price, which does not cover the immediate mark-to-market loss, so
   // the difference comes from the trader's balance. adjust_balance throws if it is not there,
   // rolling the whole operation back -- which is the correct outcome, because the alternative
   // is a position the market cannot pay out on.
   const share_type shortfall = required - equity;
   d.adjust_balance( account, -asset( shortfall, market.collateral_asset ) );
   d.modify( *itr, [&shortfall]( futures_position_object& p ) { p.margin += shortfall; } );
}

/// Emits the virtual operation that records a fill in account history.
void record_fill( database& d, const futures_market_object& market, account_id_type account,
                  bool is_long, share_type n, share_type price, bool is_maker )
{
   futures_fill_operation vop( market.get_id(), account, is_long, n, price, is_maker );
   vop.fee = asset( 0, market.collateral_asset );
   d.push_applied_operation( vop );
}

} // namespace

share_type futures_margin_required( share_type size, share_type price, uint16_t ratio )
{
   // Round up. A requirement rounded down would let a position open slightly
   // under-collateralised, and every such shortfall is ultimately the market's to absorb.
   const fc::uint128_t notional = fc::uint128_t( size.value ) * price.value;
   const fc::uint128_t numerator = notional * ratio;
   const fc::uint128_t denom( GRAPHENE_100_PERCENT );
   const fc::uint128_t rounded = ( numerator + denom - 1 ) / denom;
   FC_ASSERT( rounded <= fc::uint128_t( GRAPHENE_MAX_SHARE_SUPPLY ),
              "Required margin is out of range" );
   return share_type( static_cast<int64_t>( rounded ) );
}

void_result futures_order_create_evaluator::do_evaluate( const futures_order_create_operation& op )
{ try {
   database& d = db();
   const auto now = d.head_block_time();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( now ), "Not allowed until the futures hardfork" );

   _market = &op.market_id(d);
   FC_ASSERT( _market->is_tradable( now ),
              "Market '${s}' is not accepting orders: it is halted, settled, past expiry, or "
              "has no mark price", ("s", _market->symbol) );

   _required_margin = futures_margin_required( op.size, op.price_per_contract,
                                               _market->options.initial_margin_ratio );

   // Reserved up front rather than at fill time. An order that could not be paid for if it
   // filled has no business resting on the book.
   FC_ASSERT( d.get_balance( op.owner, _market->collateral_asset ).amount >= _required_margin,
              "Insufficient balance to reserve ${m} margin for this order",
              ("m", _required_margin) );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

object_id_type futures_order_create_evaluator::do_apply( const futures_order_create_operation& op )
{ try {
   database& d = db();
   const futures_market_object& market = *_market;

   d.adjust_balance( op.owner, -asset( _required_margin, market.collateral_asset ) );

   share_type remaining = op.size;
   share_type reserved  = _required_margin;
   share_type oi_delta  = 0;

   const auto& book = d.get_index_type<futures_order_index>().indices().get<by_market_book>();

   // A taker long lifts the lowest ask; a taker short hits the highest bid. Both fill at the
   // MAKER's price, which is what makes resting on the book worthwhile and stops a taker from
   // choosing its own fill price.
   while( remaining > 0 )
   {
      const futures_order_object* best = nullptr;
      if( op.is_long )
      {
         auto itr = book.lower_bound( boost::make_tuple( market.get_id(), false ) );
         const auto end = book.upper_bound( boost::make_tuple( market.get_id(), false ) );
         if( itr != end && itr->price_per_contract <= op.price_per_contract )
            best = &(*itr);
      }
      else
      {
         auto end = book.upper_bound( boost::make_tuple( market.get_id(), true ) );
         const auto begin = book.lower_bound( boost::make_tuple( market.get_id(), true ) );
         if( end != begin )
         {
            --end;
            if( end->price_per_contract >= op.price_per_contract )
               best = &(*end);
         }
      }
      if( nullptr == best )
         break;

      // Self-matching would let one account pay itself the spread and, worse, net to a
      // position built from two halves of its own order.
      FC_ASSERT( best->owner != op.owner,
                 "This order would match your own resting order; cancel it first" );

      const share_type fill_price = best->price_per_contract;
      const share_type n = std::min( remaining, best->size );

      // The maker's margin was reserved at its own price when it was placed.
      const share_type maker_margin = futures_margin_required(
            n, fill_price, market.options.initial_margin_ratio );
      const share_type maker_share = std::min( maker_margin, best->deferred_margin );

      // The taker reserved at its own limit price, which is not the fill price.
      //
      // A long filling below its bid needs less than it reserved, and the excess simply stays
      // reserved for the rest of the order. A SHORT filling above its ask needs MORE: its
      // notional is size x fill_price, and filling higher is precisely the good case for a
      // seller. Reserving at the limit price alone would then open the position below the
      // initial margin requirement -- which is how a market ends up holding positions it never
      // agreed to hold. The shortfall is taken from the taker's balance instead; if it cannot
      // be paid, adjust_balance throws and the whole operation is rolled back atomically,
      // which is the right outcome: better to reject the order than to open it undercollateralised.
      const share_type taker_margin = futures_margin_required(
            n, fill_price, market.options.initial_margin_ratio );
      const share_type taker_share = std::min( taker_margin, reserved );
      reserved -= taker_share;
      const share_type taker_shortfall = taker_margin - taker_share;
      if( taker_shortfall > 0 )
         d.adjust_balance( op.owner, -asset( taker_shortfall, market.collateral_asset ) );

      const account_id_type maker_account = best->owner;
      const share_type maker_remaining = best->size - n;
      const share_type maker_left_over  = best->deferred_margin - maker_share;

      if( maker_remaining > 0 )
         d.modify( *best, [&]( futures_order_object& o ) {
            o.size            = maker_remaining;
            o.deferred_margin = maker_left_over;
         } );
      else
      {
         // Any reservation the maker no longer needs goes straight back; it was never spent.
         if( maker_left_over > 0 )
            d.adjust_balance( maker_account, asset( maker_left_over, market.collateral_asset ) );
         d.remove( *best );
      }

      oi_delta += apply_fill( d, market, maker_account, !op.is_long, n, fill_price, maker_share );
      oi_delta += apply_fill( d, market, op.owner,       op.is_long, n, fill_price,
                              taker_share + taker_shortfall );

      // Both sides, every fill. A maker resting far from the mark is exposed to exactly the
      // same problem as a taker crossing to it.
      require_margin_at_mark( d, market, maker_account );
      require_margin_at_mark( d, market, op.owner );

      record_fill( d, market, maker_account, !op.is_long, n, fill_price, true );
      record_fill( d, market, op.owner,       op.is_long, n, fill_price, false );

      remaining -= n;
   }

   // Open interest counts contracts, and each contract has a long and a short, so the two
   // sides' deltas are summed and halved.
   if( oi_delta != 0 )
      d.modify( market, [&oi_delta]( futures_market_object& m ) {
         m.open_interest += oi_delta / 2;
      } );

   if( remaining > 0 && !op.fill_or_kill )
   {
      const auto& order = d.create<futures_order_object>( [&]( futures_order_object& o ) {
         o.owner              = op.owner;
         o.market_id          = market.get_id();
         o.is_long            = op.is_long;
         o.price_per_contract = op.price_per_contract;
         o.size               = remaining;
         o.deferred_margin    = reserved;
      } );
      return order.id;
   }

   // Fully filled, or killed. Whatever margin was reserved and not used goes back.
   if( reserved > 0 )
      d.adjust_balance( op.owner, asset( reserved, market.collateral_asset ) );

   return object_id_type();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_order_cancel_evaluator::do_evaluate( const futures_order_cancel_operation& op )
{ try {
   const database& d = db();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( d.head_block_time() ),
              "Not allowed until the futures hardfork" );

   _order = &op.order_id(d);
   FC_ASSERT( _order->owner == op.owner, "Only the order's owner may cancel it" );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_order_cancel_evaluator::do_apply( const futures_order_cancel_operation& op ) const
{ try {
   database& d = db();
   const futures_market_object& market = _order->market_id(d);

   if( _order->deferred_margin > 0 )
      d.adjust_balance( _order->owner,
                        asset( _order->deferred_margin, market.collateral_asset ) );
   d.remove( *_order );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE


void_result futures_position_adjust_margin_evaluator::do_evaluate(
      const futures_position_adjust_margin_operation& op )
{ try {
   const database& d = db();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( d.head_block_time() ),
              "Not allowed until the futures hardfork" );

   _position = &op.position_id(d);
   FC_ASSERT( _position->owner == op.owner, "Only the position's owner may adjust its margin" );

   if( op.delta < 0 )
   {
      const futures_market_object& market = _position->market_id(d);
      FC_ASSERT( market.mark_price.valid(),
                 "Margin cannot be withdrawn while the market has no mark price to measure "
                 "the requirement against" );
      const share_type mark = *market.mark_price;
      const share_type required = futures_margin_required( _position->abs_size(), mark,
                                                    market.options.initial_margin_ratio );
      // Measured against the INITIAL requirement, not the maintenance one: withdrawing down to
      // the liquidation threshold would leave a position one tick from being taken away.
      FC_ASSERT( _position->equity( mark ) + op.delta >= required,
                 "Withdrawing ${w} would leave equity ${e} against an initial margin "
                 "requirement of ${r}",
                 ("w", -op.delta)("e", _position->equity( mark ) + op.delta)("r", required) );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_position_adjust_margin_evaluator::do_apply(
      const futures_position_adjust_margin_operation& op ) const
{ try {
   database& d = db();
   const futures_market_object& market = _position->market_id(d);

   // Charge whatever funding has accrued first, so the margin being adjusted is the real
   // current figure rather than one that silently owes the market money.
   apply_funding( d, market, *_position );

   // Negative delta pays out, positive takes in; adjust_balance throws if the account cannot
   // fund an addition, rolling the operation back.
   d.adjust_balance( op.owner, asset( -op.delta, market.collateral_asset ) );
   d.modify( *_position, [&op]( futures_position_object& p ) { p.margin += op.delta; } );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_liquidate_evaluator::do_evaluate( const futures_liquidate_operation& op )
{ try {
   const database& d = db();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( d.head_block_time() ),
              "Not allowed until the futures hardfork" );

   _position = &op.position_id(d);
   _market   = &_position->market_id(d);

   FC_ASSERT( _market->mark_price.valid(),
              "Cannot liquidate while the market has no mark price: risk would be assessed "
              "against a price nobody is asserting" );

   const share_type mark = *_market->mark_price;
   const share_type maintenance = futures_margin_required(
         _position->abs_size(), mark, _market->options.maintenance_margin_ratio );

   FC_ASSERT( _position->equity( mark ) < maintenance,
              "Position is not liquidatable: equity ${e} still meets the maintenance "
              "requirement of ${r} at mark ${m}",
              ("e", _position->equity( mark ))("r", maintenance)("m", mark) );

   FC_ASSERT( _position->owner != op.liquidator,
              "An account cannot liquidate its own position; add margin instead" );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_liquidate_evaluator::do_apply( const futures_liquidate_operation& op ) const
{ try {
   database& d = db();
   const futures_market_object& market = *_market;
   const share_type mark = *market.mark_price;

   // Settle through the pool first, so the stored margin IS the position's equity and
   // everything below is plain arithmetic on one number that actually exists.
   settle_to_mark( d, market, *_position );

   share_type margin = _position->margin;
   const share_type abs_size = _position->abs_size();
   const account_id_type old_owner = _position->owner;

   // A gap can leave a position worth less than nothing. The insurance fund exists for exactly
   // this; if it cannot cover the whole deficit the remainder falls to the liquidator, who can
   // see it before choosing to call.
   share_type from_fund = 0;
   if( margin < 0 )
   {
      const share_type deficit = -margin;
      from_fund = market.insurance_fund > 0 ? std::min( deficit, market.insurance_fund )
                                             : share_type( 0 );
      margin = 0;
   }

   const share_type penalty = std::min(
         futures_margin_required( abs_size, mark, market.options.liquidation_penalty_ratio ),
         margin );
   const share_type owner_payout = margin - penalty;   // >= 0 by the min above
   const share_type retained     = penalty;            // stays with the position

   const share_type required = futures_margin_required( abs_size, mark,
                                                  market.options.initial_margin_ratio );
   // The liquidator tops the position up to a full initial margin and keeps the penalty, which
   // is what makes calling this worth doing.
   const share_type top_up = ( required > retained ? required - retained : share_type(0) )
                           + ( margin < 0 ? -margin - from_fund : share_type(0) );

   d.adjust_balance( op.liquidator, -asset( top_up, market.collateral_asset ) );
   if( owner_payout > 0 )
      d.adjust_balance( old_owner, asset( owner_payout, market.collateral_asset ) );

   if( from_fund > 0 )
      d.modify( market, [&from_fund]( futures_market_object& m )
                        { m.insurance_fund -= from_fund; } );

   // entry_value is already at the mark after settling, so the position changes hands with no
   // inherited unrealised PnL: the liquidator takes on a clean position plus the penalty.
   d.modify( *_position, [&]( futures_position_object& p ) {
      p.owner  = op.liquidator;
      p.margin = retained + top_up + from_fund;
   } );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE


void accrue_futures_funding( database& d, const futures_market_object& market )
{
   if( !market.is_perpetual() || market.is_settled || !market.mark_price.valid() )
      return;

   const auto now = d.head_block_time();
   if( ( now - market.last_funding_time ).to_seconds() < int64_t( market.options.funding_interval_sec ) )
      return;

   const share_type mark = *market.mark_price;

   // The premium is measured against the book's mid. With one side empty there is no mid and
   // therefore no premium: funding is skipped rather than guessed at, because a guessed
   // funding rate transfers real money between real people.
   const auto& book = d.get_index_type<futures_order_index>().indices().get<by_market_book>();
   auto bid_end   = book.upper_bound( boost::make_tuple( market.get_id(), true ) );
   const auto bid_begin = book.lower_bound( boost::make_tuple( market.get_id(), true ) );
   auto ask_itr   = book.lower_bound( boost::make_tuple( market.get_id(), false ) );
   const auto ask_end   = book.upper_bound( boost::make_tuple( market.get_id(), false ) );

   share_type per_contract = 0;
   if( bid_end != bid_begin && ask_itr != ask_end )
   {
      --bid_end;
      const share_type mid = ( bid_end->price_per_contract + ask_itr->price_per_contract ) / 2;
      share_type premium = mid - mark;

      // Cap the rate. An uncapped funding rate is a way to drain a position in one tick.
      const share_type cap = futures_margin_required( share_type(1), mark,
            uint16_t( market.options.max_funding_rate_ppm * GRAPHENE_100_PERCENT / 1000000 ) );
      if( premium > cap )  premium = cap;
      if( premium < -cap ) premium = -cap;
      per_contract = premium;
   }

   d.modify( market, [&per_contract, now]( futures_market_object& m ) {
      m.cumulative_funding += per_contract;
      m.last_funding_time   = now;
   } );
}

void_result futures_settle_evaluator::do_evaluate( const futures_settle_operation& op )
{ try {
   const database& d = db();
   const auto now = d.head_block_time();

   FC_ASSERT( HARDFORK_FUTURES_PASSED( now ), "Not allowed until the futures hardfork" );

   _market = &op.market_id(d);
   FC_ASSERT( !_market->is_perpetual(),
              "A perpetual contract never expires and cannot be settled" );
   FC_ASSERT( now >= *_market->expiry,
              "Contract '${s}' does not expire until ${e}",
              ("s", _market->symbol)("e", *_market->expiry) );
   FC_ASSERT( _market->is_settled || _market->mark_price.valid(),
              "Cannot settle: the oracle has no value to settle against" );

   if( op.position_id.valid() )
   {
      _position = &(*op.position_id)(d);
      FC_ASSERT( _position->market_id == op.market_id,
                 "Position ${p} does not belong to market '${s}'",
                 ("p", *op.position_id)("s", _market->symbol) );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) } // GCOVR_EXCL_LINE

void_result futures_settle_evaluator::do_apply( const futures_settle_operation& op ) const
{ try {
   database& d = db();

   // The first caller after expiry fixes the price everyone settles against. Snapshotting once
   // matters: settling each position against a moving oracle would hand different traders
   // different prices for the same contract.
   if( !_market->is_settled )
   {
      const share_type final_price = *_market->mark_price;
      d.modify( *_market, [&final_price]( futures_market_object& m ) {
         m.settlement_price = final_price;
         m.is_settled       = true;
      } );
   }

   if( nullptr == _position )
      return void_result();

   const share_type final_price = *_market->settlement_price;

   apply_funding( d, *_market, *_position );

   // Close at the settled price: exactly the flat-position payout, margin - entry_value, with
   // the difference passing through the pool as any other close does.
   const share_type final_entry = _position->size * final_price;
   const share_type realised    = final_entry - _position->entry_value;
   const share_type payout      = _position->margin + realised;
   const share_type abs_size    = _position->abs_size();
   const account_id_type owner  = _position->owner;

   // Open interest counts contracts, and every contract is one long and one short, so it is
   // tracked as the long side alone. Decrementing for both sides of a settled pair would take
   // it negative -- as it did, to -10, before this condition existed.
   const share_type oi_delta = _position->size > 0 ? abs_size : share_type( 0 );
   d.modify( *_market, [&realised, &oi_delta]( futures_market_object& m ) {
      m.insurance_fund -= realised;
      m.open_interest  -= oi_delta;
   } );

   if( payout > 0 )
      d.adjust_balance( owner, asset( payout, _market->collateral_asset ) );

   d.remove( *_position );

   return void_result();
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
