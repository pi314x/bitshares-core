/*
 * Futures markets: margin-traded contracts settled against an oracle.
 *
 * The mark price is the load-bearing part of stage one. Liquidation is assessed against it, so
 * the tests that matter are the ones about where it comes from, what happens when it is not
 * there, and what a market owner is and is not allowed to change underneath open positions.
 */

#include <boost/test/unit_test.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/futures_object.hpp>
#include <graphene/chain/oracle_object.hpp>
#include <graphene/protocol/futures.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_AUTO_TEST_SUITE( futures_symbol_tests )

BOOST_AUTO_TEST_CASE( valid_and_invalid_futures_symbols )
{
   BOOST_CHECK( is_valid_futures_symbol( "BTC-PERP" ) );
   BOOST_CHECK( is_valid_futures_symbol( "BTC-2026.03" ) );
   BOOST_CHECK( is_valid_futures_symbol( "ETH" ) );

   BOOST_CHECK( !is_valid_futures_symbol( "AB" ) );
   BOOST_CHECK( !is_valid_futures_symbol( "btc-perp" ) );
   BOOST_CHECK( !is_valid_futures_symbol( "BTC PERP" ) );
   BOOST_CHECK( !is_valid_futures_symbol( "-BTCPERP" ) );
   BOOST_CHECK( !is_valid_futures_symbol( "BTCPERP-" ) );
   BOOST_CHECK( !is_valid_futures_symbol( "BTC--PERP" ) );
}

/// Margin ratios are the only thing standing between a leveraged market and insolvency, so the
/// nonsensical combinations have to be rejected rather than merely discouraged.
BOOST_AUTO_TEST_CASE( margin_ratio_validation )
{
   futures_market_options ok;
   BOOST_CHECK_NO_THROW( ok.validate() );

   // maintenance at or above initial means a position is liquidatable the moment it opens
   futures_market_options equal;
   equal.initial_margin_ratio = 1000;
   equal.maintenance_margin_ratio = 1000;
   BOOST_CHECK_THROW( equal.validate(), fc::exception );

   futures_market_options inverted;
   inverted.initial_margin_ratio = 500;
   inverted.maintenance_margin_ratio = 1000;
   BOOST_CHECK_THROW( inverted.validate(), fc::exception );

   // leverage beyond the cap
   futures_market_options too_much_leverage;
   too_much_leverage.initial_margin_ratio = 10;   // 1000x
   too_much_leverage.maintenance_margin_ratio = 5;
   BOOST_CHECK_THROW( too_much_leverage.validate(), fc::exception );

   futures_market_options zero_maintenance;
   zero_maintenance.maintenance_margin_ratio = 0;
   BOOST_CHECK_THROW( zero_maintenance.validate(), fc::exception );
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

/// Shared setup: an oracle quoting a test asset against CORE, and a market on top of it.
struct futures_fixture : database_fixture
{
   asset_id_type core_id;
   asset_id_type btc_id;

   void setup_assets()
   {
      core_id = asset_id_type();
      btc_id  = create_user_issued_asset( "BTCTEST" ).get_id();
   }

   oracle_id_type make_oracle( account_id_type owner, const fc::ecc::private_key& key,
                               account_id_type producer, const string& name = "BTC.CORE" )
   {
      oracle_create_operation op;
      op.owner       = owner;
      op.name        = name;
      op.base_asset  = btc_id;
      op.quote_asset = core_id;
      op.options.producers[producer] = 1;
      op.options.minimum_producers = 1;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      return oracle_id_type { PUSH_TX( db, tx ).operation_results.front().get<object_id_type>() };
   }

   /// Publishes "one BTC is worth `core` CORE".
   void publish( oracle_id_type oid, account_id_type who, const fc::ecc::private_key& key,
                 int64_t core )
   {
      oracle_publish_operation op;
      op.producer  = who;
      op.oracle_id = oid;
      op.value     = price( asset( 1, btc_id ), asset( core, core_id ) );
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }

   /// Places an order and returns the resulting order id, or a null id if it fully filled.
   object_id_type place( futures_market_id_type mid, account_id_type who,
                         const fc::ecc::private_key& key, bool is_long,
                         int64_t price, int64_t size, bool fok = false )
   {
      futures_order_create_operation op;
      op.owner              = who;
      op.market_id          = mid;
      op.is_long            = is_long;
      op.price_per_contract = price;
      op.size               = size;
      op.fill_or_kill       = fok;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      return PUSH_TX( db, tx ).operation_results.front().get<object_id_type>();
   }

   void cancel( futures_order_id_type oid, account_id_type who, const fc::ecc::private_key& key )
   {
      futures_order_cancel_operation op;
      op.owner    = who;
      op.order_id = oid;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }

   const futures_position_object* position_of( futures_market_id_type mid, account_id_type who )
   {
      const auto& idx = db.get_index_type<futures_position_index>().indices()
                          .get<by_market_owner>();
      auto itr = idx.find( boost::make_tuple( mid, who ) );
      return itr == idx.end() ? nullptr : &(*itr);
   }

   /**
    * The solvency invariant: across every position in a market, sizes sum to zero and entry
    * values sum to zero. Total PnL is therefore identically zero at ANY mark price, so the
    * market cannot leak value however prices move. Asserted after every trading test.
    */
   void check_market_is_zero_sum( futures_market_id_type mid )
   {
      share_type total_size = 0;
      share_type total_entry = 0;
      share_type long_contracts = 0;
      const auto& idx = db.get_index_type<futures_position_index>().indices().get<by_id>();
      for( const auto& p : idx )
      {
         if( p.market_id != mid ) continue;
         total_size  += p.size;
         total_entry += p.entry_value;
         if( p.size > 0 ) long_contracts += p.size;
      }
      BOOST_CHECK_MESSAGE( 0 == total_size.value,
                           "sum of position sizes is " + std::to_string( total_size.value ) );
      BOOST_CHECK_MESSAGE( 0 == total_entry.value,
                           "sum of entry values is " + std::to_string( total_entry.value ) );
      BOOST_CHECK_EQUAL( long_contracts.value, mid(db).open_interest.value );
   }

   futures_market_id_type make_market( account_id_type owner, const fc::ecc::private_key& key,
                                       oracle_id_type oid, share_type contract_size,
                                       const optional<time_point_sec>& expiry = {},
                                       const string& symbol = "BTC-PERP" )
   {
      futures_market_create_operation op;
      op.owner            = owner;
      op.symbol           = symbol;
      op.oracle_id        = oid;
      op.collateral_asset = core_id;
      op.contract_size    = contract_size;
      op.expiry           = expiry;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      return futures_market_id_type {
         PUSH_TX( db, tx ).operation_results.front().get<object_id_type>() };
   }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE( futures_tests, futures_fixture )

BOOST_AUTO_TEST_CASE( futures_ops_are_refused_before_the_hardfork )
{ try {
   // past the oracle fork so an oracle can exist, but not the futures fork
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );

   // The futures fork is deliberately later than the oracle fork: a market's mark price comes
   // from an oracle, so futures cannot activate first. This asserts that ordering as well as
   // the gate itself -- if the two dates were ever set equal, this test would say so.
   BOOST_REQUIRE( HARDFORK_FUTURES_TIME > HARDFORK_ORACLE_TIME );
   BOOST_REQUIRE( !HARDFORK_FUTURES_PASSED( db.head_block_time() ) );
   GRAPHENE_REQUIRE_THROW( make_market( alice_id, alice_private_key, oid, 1 ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// The mark price is an integer amount of collateral per contract, derived from the oracle's
/// ratio and the contract size. This is the one conversion in the whole design.
BOOST_AUTO_TEST_CASE( the_mark_price_comes_from_the_oracle_and_the_contract_size )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 50000 );   // 1 BTC = 50000 CORE

   // a contract worth 10 units of the base asset
   const auto mid = make_market( alice_id, alice_private_key, oid, 10 );

   BOOST_REQUIRE( mid(db).mark_price.valid() );
   BOOST_CHECK_EQUAL( mid(db).mark_price->value, 500000 );   // 10 x 50000
   BOOST_CHECK( mid(db).is_tradable( db.head_block_time() ) );

   // a new oracle value moves the mark in the same block it is published
   publish( oid, bob_id, bob_private_key, 60000 );
   BOOST_REQUIRE( mid(db).mark_price.valid() );
   BOOST_CHECK_EQUAL( mid(db).mark_price->value, 600000 );
} FC_LOG_AND_RETHROW() }

/// Without a mark there is no risk measure, so the market must stop rather than trade against
/// a price nobody is asserting.
BOOST_AUTO_TEST_CASE( a_market_without_a_mark_price_is_not_tradable )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   // an oracle that has never had a value published to it
   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   BOOST_CHECK( !mid(db).mark_price.valid() );
   BOOST_CHECK( !mid(db).is_tradable( db.head_block_time() ) );

   // once a value exists the market comes to life
   publish( oid, bob_id, bob_private_key, 1000 );
   BOOST_REQUIRE( mid(db).mark_price.valid() );
   BOOST_CHECK( mid(db).is_tradable( db.head_block_time() ) );

   // and if the oracle loses quorum the mark goes away again rather than going stale
   oracle_options no_producers;
   no_producers.producers.clear();
   no_producers.minimum_producers = 1;
   oracle_update_operation uop;
   uop.owner       = alice_id;
   uop.oracle_id   = oid;
   uop.new_options = no_producers;
   signed_transaction tx;
   tx.operations.push_back( uop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   PUSH_TX( db, tx );

   BOOST_CHECK( !oid(db).current_value.valid() );
   BOOST_CHECK( !mid(db).mark_price.valid() );
   BOOST_CHECK( !mid(db).is_tradable( db.head_block_time() ) );
} FC_LOG_AND_RETHROW() }

/// Margin and PnL are in the collateral asset; the oracle must quote against that same asset
/// so no unit conversion -- and therefore no extra rounding -- ever enters position accounting.
BOOST_AUTO_TEST_CASE( the_oracle_must_quote_against_the_collateral_asset )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto other_id = create_user_issued_asset( "OTHERTEST" ).get_id();

   // oracle quotes BTC/OTHER, but the market wants to margin in CORE
   oracle_create_operation oop;
   oop.owner       = alice_id;
   oop.name        = "BTC.OTHER";
   oop.base_asset  = btc_id;
   oop.quote_asset = other_id;
   oop.options.producers[bob_id] = 1;
   oop.options.minimum_producers = 1;
   signed_transaction otx;
   otx.operations.push_back( oop );
   db.current_fee_schedule().set_fee( otx.operations.back() );
   set_expiration( db, otx );
   otx.sign( alice_private_key, db.get_chain_id() );
   const oracle_id_type mismatched {
      PUSH_TX( db, otx ).operation_results.front().get<object_id_type>() };

   GRAPHENE_REQUIRE_THROW( make_market( alice_id, alice_private_key, mismatched, 1 ),
                           fc::exception );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( futures_symbols_are_unique_and_only_the_owner_may_update )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 1000 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // same symbol, different owner
   GRAPHENE_REQUIRE_THROW( make_market( bob_id, bob_private_key, oid, 1 ), fc::exception );

   auto update_as = [&]( account_id_type who, const fc::ecc::private_key& key ) {
      futures_market_update_operation op;
      op.owner       = who;
      op.market_id   = mid;
      futures_market_options opts = mid(db).options;
      opts.enabled = false;
      op.new_options = opts;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   };

   GRAPHENE_REQUIRE_THROW( update_as( bob_id, bob_private_key ), fc::exception );

   update_as( alice_id, alice_private_key );
   BOOST_CHECK( !mid(db).options.enabled );
   // halting a market stops trading without destroying anything
   BOOST_CHECK( !mid(db).is_tradable( db.head_block_time() ) );
} FC_LOG_AND_RETHROW() }

/// A market's oracle is fixed at creation, so unlike a smartcoin there is no "unbind first".
/// Deleting the oracle would strand the market without a mark price permanently.
BOOST_AUTO_TEST_CASE( an_oracle_feeding_a_futures_market_cannot_be_deleted )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 1000 );
   make_market( alice_id, alice_private_key, oid, 1 );

   oracle_delete_operation dop;
   dop.owner     = alice_id;
   dop.oracle_id = oid;
   signed_transaction tx;
   tx.operations.push_back( dop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, tx ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// Expiry is a promise to the people holding the contract.
BOOST_AUTO_TEST_CASE( expiry_must_be_in_the_future_and_within_range )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 1000 );
   const auto now = db.head_block_time();

   GRAPHENE_REQUIRE_THROW(
      make_market( alice_id, alice_private_key, oid, 1, now - 1, "BTC-PAST" ), fc::exception );
   GRAPHENE_REQUIRE_THROW(
      make_market( alice_id, alice_private_key, oid, 1,
                   now + fc::days( GRAPHENE_FUTURES_MAX_EXPIRY_DAYS + 1 ), "BTC-FAR" ),
      fc::exception );

   const auto dated = make_market( alice_id, alice_private_key, oid, 1,
                                   now + fc::days( 30 ), "BTC-2026" );
   BOOST_CHECK( !dated(db).is_perpetual() );
   BOOST_CHECK( dated(db).is_tradable( db.head_block_time() ) );

   // past its expiry a dated contract stops accepting trades
   BOOST_CHECK( !dated(db).is_tradable( *dated(db).expiry ) );
   BOOST_CHECK( !dated(db).is_tradable( *dated(db).expiry + 1 ) );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()


BOOST_FIXTURE_TEST_SUITE( futures_trading_tests, futures_fixture )

namespace {
   constexpr int64_t IMR = 1000;   // 10% initial margin, the futures_market_options default
}

/// A crossing pair of orders creates one long and one short at the maker's price.
BOOST_AUTO_TEST_CASE( a_crossing_pair_opens_two_positions )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice ); fund( bob ); fund( carol );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   const auto bob_before   = db.get_balance( bob_id, core_id ).amount;
   const auto carol_before = db.get_balance( carol_id, core_id ).amount;

   // bob rests a bid for 10 contracts at 100; carol sells into it at 90
   place( mid, bob_id, bob_private_key, true, 100, 10 );
   BOOST_CHECK( nullptr == position_of( mid, bob_id ) );   // nothing crossed yet
   place( mid, carol_id, carol_private_key, false, 90, 10 );

   const auto* bob_pos   = position_of( mid, bob_id );
   const auto* carol_pos = position_of( mid, carol_id );
   BOOST_REQUIRE( nullptr != bob_pos );
   BOOST_REQUIRE( nullptr != carol_pos );

   // filled at the MAKER's price of 100, not the taker's 90
   BOOST_CHECK_EQUAL( bob_pos->size.value, 10 );
   BOOST_CHECK_EQUAL( bob_pos->entry_value.value, 1000 );
   BOOST_CHECK_EQUAL( carol_pos->size.value, -10 );
   BOOST_CHECK_EQUAL( carol_pos->entry_value.value, -1000 );

   // 10% of the 1000 notional, for both sides. Carol asked 90 and so reserved only 90; she
   // filled at 100, which is the better price for a seller but a larger notional, and the
   // extra 10 was taken from her balance rather than letting her open under-margined.
   BOOST_CHECK_EQUAL( bob_pos->margin.value, 100 );
   BOOST_CHECK_EQUAL( carol_pos->margin.value, 100 );

   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 10 );
   check_market_is_zero_sum( mid );

   // margin actually left their balances (fees aside, which are charged in core too, so
   // compare the margin component by checking it is at least the margin)
   BOOST_CHECK( db.get_balance( bob_id, core_id ).amount <= bob_before - 100 );
   BOOST_CHECK( db.get_balance( carol_id, core_id ).amount <= carol_before - 100 );
} FC_LOG_AND_RETHROW() }

/// Closing a position pays out margin plus realised PnL exactly, with no rounding.
BOOST_AUTO_TEST_CASE( closing_a_position_pays_out_margin_plus_pnl )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // open: bob long 10 @ 100, carol short 10 @ 100
   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   check_market_is_zero_sum( mid );

   // close at 120: bob sells 10, carol buys 10. Bob is up 10 x (120-100) = 200.
   const auto bob_before = db.get_balance( bob_id, core_id ).amount;
   place( mid, carol_id, carol_private_key, true, 120, 10 );   // carol bids to close
   place( mid, bob_id, bob_private_key, false, 120, 10 );      // bob sells into it

   BOOST_CHECK( nullptr == position_of( mid, bob_id ) );
   BOOST_CHECK( nullptr == position_of( mid, carol_id ) );
   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 0 );

   // bob gets back his 100 margin plus 200 profit; the fee is separate and small
   const auto bob_gain = db.get_balance( bob_id, core_id ).amount - bob_before;
   BOOST_CHECK_MESSAGE( bob_gain > 250 && bob_gain <= 300,
                        "bob's payout was " + std::to_string( bob_gain.value ) );

   check_market_is_zero_sum( mid );
} FC_LOG_AND_RETHROW() }

/// A partial fill leaves the remainder resting, and cancelling returns exactly what is left.
BOOST_AUTO_TEST_CASE( partial_fills_rest_and_cancel_returns_the_reservation )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // bob bids 10 @ 100; carol sells only 4
   const auto raw = place( mid, bob_id, bob_private_key, true, 100, 10 );
   const futures_order_id_type bob_order { raw };
   place( mid, carol_id, carol_private_key, false, 100, 4 );

   BOOST_REQUIRE( nullptr != db.find( bob_order ) );
   BOOST_CHECK_EQUAL( bob_order(db).size.value, 6 );
   // 10 contracts were reserved at 10%; 4 filled, so 60 of the 100 remains reserved
   BOOST_CHECK_EQUAL( bob_order(db).deferred_margin.value, 60 );

   const auto* bob_pos = position_of( mid, bob_id );
   BOOST_REQUIRE( nullptr != bob_pos );
   BOOST_CHECK_EQUAL( bob_pos->size.value, 4 );
   BOOST_CHECK_EQUAL( bob_pos->margin.value, 40 );
   check_market_is_zero_sum( mid );

   const auto before = db.get_balance( bob_id, core_id ).amount;
   cancel( bob_order, bob_id, bob_private_key );
   BOOST_CHECK( nullptr == db.find( bob_order ) );
   BOOST_CHECK_EQUAL( ( db.get_balance( bob_id, core_id ).amount - before ).value, 60 );
} FC_LOG_AND_RETHROW() }

/// Reducing a position must not open a hedged pair, or a trader would pay margin twice to be
/// flat.
BOOST_AUTO_TEST_CASE( trading_the_other_way_reduces_rather_than_hedges )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   BOOST_CHECK_EQUAL( position_of( mid, bob_id )->size.value, 10 );

   // bob sells 4 back; carol takes the other side, reducing hers too
   place( mid, bob_id, bob_private_key, false, 100, 4 );
   place( mid, carol_id, carol_private_key, true, 100, 4 );

   BOOST_CHECK_EQUAL( position_of( mid, bob_id )->size.value, 6 );
   BOOST_CHECK_EQUAL( position_of( mid, carol_id )->size.value, -6 );
   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 6 );
   check_market_is_zero_sum( mid );
} FC_LOG_AND_RETHROW() }

/// Self-matching would let one account pay itself the spread and build a position out of two
/// halves of its own order.
BOOST_AUTO_TEST_CASE( an_order_cannot_match_its_owners_own_order )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   GRAPHENE_REQUIRE_THROW( place( mid, bob_id, bob_private_key, false, 90, 10 ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// Margin is reserved when the order is placed, not when it fills.
BOOST_AUTO_TEST_CASE( an_order_that_cannot_be_paid_for_is_refused )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( dan, asset(500) );   // enough for fees, nowhere near enough margin

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // 1,000,000 contracts at 100 needs 10,000,000 margin
   GRAPHENE_REQUIRE_THROW( place( mid, dan_id, dan_private_key, true, 100, 1000000 ),
                           fc::exception );
} FC_LOG_AND_RETHROW() }

/// fill_or_kill discards the remainder instead of resting it, and refunds its reservation.
BOOST_AUTO_TEST_CASE( fill_or_kill_does_not_rest )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 4 );

   const auto before = db.get_balance( carol_id, core_id ).amount;
   place( mid, carol_id, carol_private_key, false, 100, 10, true );   // FOK

   // only 4 could fill; nothing rests
   BOOST_CHECK_EQUAL( position_of( mid, carol_id )->size.value, -4 );
   const auto& book = db.get_index_type<futures_order_index>().indices().get<by_id>();
   size_t resting = 0;
   for( const auto& o : book ) if( o.market_id == mid ) ++resting;
   BOOST_CHECK_EQUAL( resting, 0u );

   // she reserved for 10 and used 40; the other 60 came back
   BOOST_CHECK_EQUAL( ( before - db.get_balance( carol_id, core_id ).amount ).value, 40 );
   check_market_is_zero_sum( mid );
} FC_LOG_AND_RETHROW() }

/// A halted or unmarked market must not accept orders.
BOOST_AUTO_TEST_CASE( orders_are_refused_when_the_market_is_not_tradable )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // no mark price yet
   BOOST_REQUIRE( !mid(db).mark_price.valid() );
   GRAPHENE_REQUIRE_THROW( place( mid, bob_id, bob_private_key, true, 100, 1 ), fc::exception );

   publish( oid, bob_id, bob_private_key, 100 );
   BOOST_CHECK_NO_THROW( place( mid, bob_id, bob_private_key, true, 100, 1 ) );

   // owner halts the market
   futures_market_update_operation uop;
   uop.owner     = alice_id;
   uop.market_id = mid;
   futures_market_options halted = mid(db).options;
   halted.enabled = false;
   uop.new_options = halted;
   signed_transaction tx;
   tx.operations.push_back( uop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   PUSH_TX( db, tx );

   GRAPHENE_REQUIRE_THROW( place( mid, bob_id, bob_private_key, true, 100, 1 ), fc::exception );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
