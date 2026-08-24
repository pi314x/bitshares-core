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
                               account_id_type producer, const string& name = "BTC.CORE",
                               const optional<uint32_t>& value_lifetime_sec = {} )
   {
      oracle_create_operation op;
      op.owner       = owner;
      op.name        = name;
      op.base_asset  = btc_id;
      op.quote_asset = core_id;
      op.options.producers[producer] = 1;
      op.options.minimum_producers = 1;
      if( value_lifetime_sec.valid() )
         op.options.value_lifetime_sec = *value_lifetime_sec;
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

   void adjust_margin( futures_position_id_type pid, account_id_type who,
                       const fc::ecc::private_key& key, int64_t delta )
   {
      futures_position_adjust_margin_operation op;
      op.owner       = who;
      op.position_id = pid;
      op.delta       = delta;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }

   void liquidate( futures_position_id_type pid, account_id_type who,
                   const fc::ecc::private_key& key )
   {
      futures_liquidate_operation op;
      op.liquidator  = who;
      op.position_id = pid;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }

   void settle_market( futures_market_id_type mid, account_id_type who,
                       const fc::ecc::private_key& key,
                       const optional<futures_position_id_type>& pid = {} )
   {
      futures_settle_operation op;
      op.payer       = who;
      op.market_id   = mid;
      op.position_id = pid;
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
    * Every contract has a long and a short, so sizes must net to zero and open interest must
    * equal the long side. Deliberately NOT asserting that entry values sum to zero: that is
    * only true while closes are symmetric, and believing it hid a real insolvency. Conservation
    * of collateral is checked by verify_asset_supplies, which the fixture runs anyway.
    */
   void check_market_is_balanced( futures_market_id_type mid )
   {
      share_type total_size = 0;
      share_type long_contracts = 0;
      const auto& idx = db.get_index_type<futures_position_index>().indices().get<by_id>();
      for( const auto& p : idx )
      {
         if( p.market_id != mid ) continue;
         total_size += p.size;
         if( p.size > 0 ) long_contracts += p.size;
      }
      BOOST_CHECK_MESSAGE( 0 == total_size.value,
                           "sum of position sizes is " + std::to_string( total_size.value ) );
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
   check_market_is_balanced( mid );

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
   check_market_is_balanced( mid );

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

   check_market_is_balanced( mid );
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
   check_market_is_balanced( mid );

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
   check_market_is_balanced( mid );
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
   check_market_is_balanced( mid );
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


/**
 * A premium held for a moment must not move funding as much as one held all interval.
 *
 * Funding used to read the book mid ONCE, at the instant the interval elapsed. Whichever two
 * orders happened to be best at that moment set the rate for the whole interval, so on a thin
 * book a single non-marketable order placed just before the sample -- and cancelled just after
 * -- moved the mid as far as the cap allowed. That is a repeatable transfer from one side of
 * the market to the other, every interval, for the price of an order that never fills.
 *
 * The premium is now time-weighted across the interval, so a quote only counts for as long as
 * it is actually exposed. This runs the same skew twice: held briefly, then held throughout.
 */
BOOST_AUTO_TEST_CASE( a_momentary_quote_cannot_set_the_funding_rate )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   // A mark large enough that the rate cap is not a single unit. At a mark of 100 the cap is
   // ceil(100 * 750/1e6) == 1, so a held premium and a momentary one both saturate it and the
   // two are indistinguishable at integer resolution -- the measurement, not the behaviour.
   const int64_t oracle_price = 10000000;
   fund( alice, asset( 1000000000000 ) );
   fund( bob,   asset( 1000000000000 ) );
   fund( carol, asset( 1000000000000 ) );

   const auto oid = make_oracle( alice_id, alice_private_key, alice_id, "FUND.CORE" );
   publish( oid, alice_id, alice_private_key, oracle_price );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );
   BOOST_REQUIRE( mid(db).is_perpetual() );

   const int64_t mark = mid(db).mark_price->value;
   const uint32_t interval = mid(db).options.funding_interval_sec;

   // A thin book centred on the mark, so the baseline premium is zero and any movement is the
   // skew and nothing else. Deliberately NOT a very wide book: with bids and asks far from the
   // mark the mid alone already exceeds the rate cap, and every reading saturates whatever the
   // sampler does -- which would make this test pass for the wrong reason.
   const int64_t cap    = ( mark * 750 + 999999 ) / 1000000;   // futures_ppm_of, rounded up
   const int64_t spread = cap * 4;
   place( mid, bob_id,   bob_private_key,   true,  mark - spread, 1 );
   place( mid, carol_id, carol_private_key, false, mark + spread, 1 );
   BOOST_REQUIRE_GT( cap, 1 );   // otherwise the two runs cannot differ at integer resolution

   // --- run one: skew the book for a single block near the end of the interval ---------
   generate_blocks( db.head_block_time() + fc::seconds( interval - 10 ) );
   set_expiration( db, trx );
   const futures_order_id_type skew_order {
      place( mid, bob_id, bob_private_key, true, mark + spread - 1, 1 ) };
   publish( oid, alice_id, alice_private_key, oracle_price );   // sample the skew
   cancel( skew_order, bob_id, bob_private_key );

   const auto before_brief = mid(db).cumulative_funding;
   generate_blocks( db.head_block_time() + fc::seconds( 20 ) );
   set_expiration( db, trx );
   publish( oid, alice_id, alice_private_key, oracle_price );   // closes the interval
   const auto brief_funding = mid(db).cumulative_funding - before_brief;

   // --- run two: the same skew, held for the whole interval ----------------------------
   const futures_order_id_type held_order {
      place( mid, bob_id, bob_private_key, true, mark + spread - 1, 1 ) };
   const auto before_held = mid(db).cumulative_funding;
   // Publish periodically, as a live oracle would, so the premium is sampled across the span.
   for( int i = 0; i < 4; ++i )
   {
      generate_blocks( db.head_block_time() + fc::seconds( interval / 4 ) );
      set_expiration( db, trx );
      publish( oid, alice_id, alice_private_key, oracle_price );
   }
   const auto held_funding = mid(db).cumulative_funding - before_held;
   cancel( held_order, bob_id, bob_private_key );

   BOOST_TEST_MESSAGE( "funding from a momentary skew: " << brief_funding.value
                       << ", from a skew held all interval: " << held_funding.value );

   // Holding the quote must cost the market more than flashing it. Under the old sampler the
   // two were identical, because only the final instant was ever read.
   BOOST_CHECK_GT( held_funding.value, brief_funding.value );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()


BOOST_FIXTURE_TEST_SUITE( futures_invariant_tests, futures_fixture )

/**
 * An asymmetric close: one side exits against a fresh counterparty while the original other
 * side stays open. This is the case that shows what the market-wide invariants actually are.
 *
 * Sum(size) is zero always. Sum(entry_value) is NOT: when a position closes, its realised PnL
 * leaves as cash, and what remains in Sum(entry_value) is exactly the negative of everything
 * paid out so far. Conservation of value is therefore not an entry_value identity -- it is
 * checked by verify_asset_supplies, which accounts for every unit of collateral held in
 * positions, resting orders and the insurance fund.
 */
BOOST_AUTO_TEST_CASE( sum_of_sizes_is_zero_but_entry_values_track_realised_pnl )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // bob long 10 @ 100 against carol short 10 @ 100
   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );

   share_type total_size = 0, total_entry = 0;
   auto tally = [&]() {
      total_size = 0; total_entry = 0;
      const auto& idx = db.get_index_type<futures_position_index>().indices().get<by_id>();
      for( const auto& p : idx )
         if( p.market_id == mid ) { total_size += p.size; total_entry += p.entry_value; }
   };

   tally();
   BOOST_CHECK_EQUAL( total_size.value, 0 );
   BOOST_CHECK_EQUAL( total_entry.value, 0 );   // nothing has closed yet

   // bob exits at 120 against dan, who opens a fresh long. Carol stays short.
   place( mid, dan_id, dan_private_key, true, 120, 10 );
   place( mid, bob_id, bob_private_key, false, 120, 10 );

   BOOST_CHECK( nullptr == position_of( mid, bob_id ) );   // bob is out, paid his 200 profit

   tally();
   // sizes still net to zero: dan is long 10, carol short 10
   BOOST_CHECK_EQUAL( total_size.value, 0 );
   BOOST_CHECK_EQUAL( position_of( mid, dan_id )->size.value, 10 );
   BOOST_CHECK_EQUAL( position_of( mid, carol_id )->size.value, -10 );

   // Because every fill settles the position to the mark, entry values are exactly size x mark
   // afterwards and net to zero again -- dan +1000, carol -1000 at a mark of 100. Dan's loss
   // from buying 20 above the mark was COLLECTED into his margin rather than left unrealised,
   // which is what funds bob's payout. Collateral conservation is checked by
   // verify_asset_supplies, which the fixture runs at the end of this test.
   BOOST_CHECK_EQUAL( total_entry.value, 0 );
   BOOST_CHECK_EQUAL( position_of( mid, dan_id )->unrealized_pnl( 100 ).value, 0 );
   BOOST_CHECK_EQUAL( position_of( mid, carol_id )->unrealized_pnl( 100 ).value, 0 );

   // dan paid for the bad entry: 120 reserved by his order plus 180 topped up = 300 in, and he
   // holds 100 of margin. The 200 difference is exactly bob's profit.
   BOOST_CHECK_EQUAL( position_of( mid, dan_id )->margin.value, 100 );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()


BOOST_FIXTURE_TEST_SUITE( futures_risk_tests, futures_fixture )

/// Opening a pair, then setting up so one side is under water at the mark.
struct opened_pair
{
   futures_market_id_type mid;
   oracle_id_type oid;
};

/// Margin can be added freely; withdrawal is bounded by the INITIAL requirement, not the
/// maintenance one, so a trader cannot withdraw down to the edge of liquidation.
BOOST_AUTO_TEST_CASE( margin_can_be_added_and_withdrawn_within_the_initial_requirement )
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

   const auto pid = position_of( mid, bob_id )->get_id();
   BOOST_CHECK_EQUAL( pid(db).margin.value, 100 );

   adjust_margin( pid, bob_id, bob_private_key, 50 );
   BOOST_CHECK_EQUAL( pid(db).margin.value, 150 );

   // requirement is 10 x 100 x 10% = 100, so 50 may come back out
   adjust_margin( pid, bob_id, bob_private_key, -50 );
   BOOST_CHECK_EQUAL( pid(db).margin.value, 100 );

   // but not a satoshi more
   GRAPHENE_REQUIRE_THROW( adjust_margin( pid, bob_id, bob_private_key, -1 ), fc::exception );

   // and not by anyone else
   GRAPHENE_REQUIRE_THROW( adjust_margin( pid, carol_id, carol_private_key, 10 ), fc::exception );

   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/// A healthy position must not be liquidatable. This is the check that stops liquidation being
/// used as a weapon.
BOOST_AUTO_TEST_CASE( a_healthy_position_cannot_be_liquidated )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );

   const auto pid = position_of( mid, bob_id )->get_id();
   GRAPHENE_REQUIRE_THROW( liquidate( pid, dan_id, dan_private_key ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// The mark moving against a leveraged long eventually puts it under the maintenance
/// requirement, and then anyone may take it over.
BOOST_AUTO_TEST_CASE( an_underwater_position_is_liquidated_and_handed_to_the_liquidator )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // bob long 10 @ 100 with 100 margin, 10x leverage
   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   const auto pid = position_of( mid, bob_id )->get_id();

   // the mark drops to 92: bob is down 80, equity 20, maintenance is 10 x 92 x 5% = 46
   publish( oid, bob_id, bob_private_key, 92 );
   BOOST_REQUIRE( mid(db).mark_price.valid() );
   BOOST_CHECK_EQUAL( mid(db).mark_price->value, 92 );
   BOOST_CHECK_EQUAL( pid(db).equity( 92 ).value, 20 );

   const auto bob_before = db.get_balance( bob_id, core_id ).amount;
   const auto dan_before = db.get_balance( dan_id, core_id ).amount;

   liquidate( pid, dan_id, dan_private_key );

   // Only as much as it takes. Equity is 20 against a maintenance requirement of 46, and the
   // owner keeps everything but the penalty on the part taken, so the smallest t satisfying
   //     20 - ceil(t x 92 x 1%)  >=  ceil((10-t) x 92 x 10%)
   // is 9: keeps 11, needs 10. Taking all ten would have cost bob the whole position and
   // charged the penalty on the whole notional to fix a shortfall of 26.
   BOOST_REQUIRE( nullptr != db.find( pid ) );
   BOOST_CHECK( pid(db).owner == bob_id );                   // bob keeps his remainder
   BOOST_CHECK_EQUAL( pid(db).size.value, 1 );
   BOOST_CHECK_EQUAL( pid(db).entry_value.value, 92 );        // its share, at the mark
   BOOST_CHECK_EQUAL( pid(db).unrealized_pnl( 92 ).value, 0 );

   // ... and what he keeps is healthy at a FULL initial margin, which is the point of
   // choosing t this way rather than merely clearing the maintenance line.
   BOOST_CHECK_EQUAL( pid(db).margin.value, 11 );
   BOOST_CHECK( pid(db).equity( 92 ) >= 10 );                 // 1 x 92 x 10%, rounded up

   // dan holds the nine contracts he took, at the mark, on a full initial margin.
   const auto* dan_pos = position_of( mid, dan_id );
   BOOST_REQUIRE( nullptr != dan_pos );
   BOOST_CHECK_EQUAL( dan_pos->size.value, 9 );
   BOOST_CHECK_EQUAL( dan_pos->entry_value.value, 9 * 92 );
   BOOST_CHECK_EQUAL( dan_pos->margin.value, ( 9 * 92 * 1000 + 9999 ) / 10000 );

   // bob is not paid out: he still holds his position, so nothing is returned to his balance.
   BOOST_CHECK_EQUAL( ( db.get_balance( bob_id, core_id ).amount - bob_before ).value, 0 );

   // dan posted the initial margin on what he took, less the penalty he earned for taking it.
   BOOST_CHECK( db.get_balance( dan_id, core_id ).amount < dan_before );

   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/// Adding margin is the defence against liquidation, and it must actually work.
BOOST_AUTO_TEST_CASE( adding_margin_prevents_liquidation )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   const auto pid = position_of( mid, bob_id )->get_id();

   publish( oid, bob_id, bob_private_key, 92 );
   // bob tops up before anyone gets to him
   adjust_margin( pid, bob_id, bob_private_key, 200 );

   GRAPHENE_REQUIRE_THROW( liquidate( pid, dan_id, dan_private_key ), fc::exception );
   BOOST_CHECK( pid(db).owner == bob_id );
   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/// Liquidating your own position would let a trader take the penalty from themselves and
/// reset their entry at the mark.
BOOST_AUTO_TEST_CASE( an_account_cannot_liquidate_itself )
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
   const auto pid = position_of( mid, bob_id )->get_id();

   publish( oid, bob_id, bob_private_key, 92 );
   GRAPHENE_REQUIRE_THROW( liquidate( pid, bob_id, bob_private_key ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// A short is liquidated by the mark moving UP, symmetrically.
BOOST_AUTO_TEST_CASE( a_short_is_liquidated_when_the_mark_rises )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   const auto cpid = position_of( mid, carol_id )->get_id();
   BOOST_CHECK_EQUAL( cpid(db).size.value, -10 );

   // mark up to 108: carol is down 80, equity 20, maintenance 10 x 108 x 5% = 54
   publish( oid, bob_id, bob_private_key, 108 );
   BOOST_CHECK_EQUAL( cpid(db).equity( 108 ).value, 20 );

   liquidate( cpid, dan_id, dan_private_key );
   BOOST_CHECK( cpid(db).owner == dan_id );
   BOOST_CHECK_EQUAL( cpid(db).size.value, -10 );
   BOOST_CHECK_EQUAL( cpid(db).unrealized_pnl( 108 ).value, 0 );

   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/**
 * Regression: a mark whose oracle has gone quiet must stop counting as a mark.
 *
 * market.mark_price is written when a producer publishes and never revisited, so it does not
 * decay. mark_price_time was recorded and read by nothing -- every consumer asked
 * mark_price.valid(), which stays true for ever once set. An oracle that stopped publishing
 * therefore froze the mark, and with it froze margin, liquidation and settlement.
 *
 * Settlement is the one that cannot be undone: it fixes a single price for everyone in the
 * market, and the first caller after expiry snapshotted whatever frozen number was sitting
 * there. A contract whose oracle died weeks before expiry settled the whole market at a
 * weeks-old price.
 */
BOOST_AUTO_TEST_CASE( a_stale_oracle_stops_marking_liquidating_and_settling )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   const uint32_t lifetime = 60;
   const auto oid = make_oracle( alice_id, alice_private_key, bob_id, "STALE.CORE", lifetime );
   publish( oid, bob_id, bob_private_key, 100 );

   const auto expiry = db.head_block_time() + fc::seconds( 30 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1, expiry, "BTC-STALE" );
   BOOST_REQUIRE( mid(db).mark_price.valid() );

   // A matched pair, opened while the oracle is still live.
   place( mid, alice_id, alice_private_key, true,  100, 10 );
   place( mid, bob_id,   bob_private_key,   false, 100, 10 );
   BOOST_REQUIRE_GT( mid(db).open_interest.value, 0 );
   const auto* long_pos = position_of( mid, alice_id );
   BOOST_REQUIRE( nullptr != long_pos );
   const auto long_pos_id = long_pos->get_id();

   // Nobody publishes again. Let the contract expire and the value age out.
   generate_blocks( db.head_block_time() + fc::seconds( lifetime * 3 ) );
   set_expiration( db, trx );
   const auto now = db.head_block_time();

   // The cached mark is untouched -- that is the whole point. Freshness cannot be read off it.
   BOOST_CHECK( mid(db).mark_price.valid() );
   BOOST_CHECK( !oid(db).is_value_live( now ) );
   BOOST_CHECK( now >= *mid(db).expiry );

   // Settling here would fix a stale price for every position in the market, for ever.
   GRAPHENE_REQUIRE_THROW( settle_market( mid, alice_id, alice_private_key ), fc::exception );
   BOOST_CHECK( !mid(db).is_settled );

   // Liquidation assesses risk against the mark, so it must refuse too.
   GRAPHENE_REQUIRE_THROW( liquidate( long_pos_id, bob_id, bob_private_key ), fc::exception );

   // A fresh publish restores all of it -- the market is stalled, not bricked.
   publish( oid, bob_id, bob_private_key, 100 );
   BOOST_CHECK( oid(db).is_value_live( db.head_block_time() ) );
   settle_market( mid, alice_id, alice_private_key );
   BOOST_CHECK( mid(db).is_settled );
} FC_LOG_AND_RETHROW() }


/**
 * Regression: a liquidator who already holds a position in the same market.
 *
 * Positions are unique per (market, owner) and liquidation reassigned owner, so this collided
 * on the index and took the node down with SIGABRT -- an ordinary operation aborting every
 * node that processed the block. The liquidated position is merged into the existing one now.
 *
 * Both directions matter. Merging like signs just adds contracts. Merging opposite signs nets
 * them off, which genuinely retires contracts and has to come out of open interest, or the
 * market reports more open contracts than exist.
 */
BOOST_AUTO_TEST_CASE( a_liquidator_may_already_hold_a_position_in_the_market )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // bob long 10 against carol; dan long 4 against alice, so dan already holds a position
   place( mid, bob_id,   bob_private_key,   true,  100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   place( mid, dan_id,   dan_private_key,   true,  100, 4 );
   place( mid, alice_id, alice_private_key, false, 100, 4 );

   const auto pid = position_of( mid, bob_id )->get_id();
   BOOST_REQUIRE( nullptr != position_of( mid, dan_id ) );
   const auto oi_before = mid(db).open_interest;
   BOOST_CHECK_EQUAL( oi_before.value, 14 );

   // equity 20 against a maintenance requirement of 10 x 92 x 5% = 46
   publish( oid, bob_id, bob_private_key, 92 );
   BOOST_REQUIRE_EQUAL( pid(db).equity( 92 ).value, 20 );

   liquidate( pid, dan_id, dan_private_key );

   // Merged, not collided. Liquidation is partial, so bob keeps 1 of his 10 and dan's own
   // long of 4 absorbs the 9 taken.
   BOOST_REQUIRE( nullptr != db.find( pid ) );
   BOOST_CHECK( pid(db).owner == bob_id );
   BOOST_CHECK_EQUAL( pid(db).size.value, 1 );
   const auto* dan_pos = position_of( mid, dan_id );
   BOOST_REQUIRE( nullptr != dan_pos );
   BOOST_CHECK_EQUAL( dan_pos->size.value, 13 );

   // Like signs, so nothing was retired.
   BOOST_CHECK_EQUAL( mid(db).open_interest.value, oi_before.value );
   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/// The other direction: the liquidator's own position is on the OPPOSITE side, so taking the
/// liquidated one over nets contracts off and open interest must fall to match.
BOOST_AUTO_TEST_CASE( liquidating_into_an_opposing_position_nets_open_interest_down )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1 );

   // bob long 10 against carol; dan SHORT 4 against alice
   place( mid, bob_id,   bob_private_key,   true,  100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   place( mid, alice_id, alice_private_key, true,  100, 4 );
   place( mid, dan_id,   dan_private_key,   false, 100, 4 );

   const auto pid = position_of( mid, bob_id )->get_id();
   BOOST_REQUIRE( nullptr != position_of( mid, dan_id ) );
   BOOST_REQUIRE_LT( position_of( mid, dan_id )->size.value, 0 );
   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 14 );

   publish( oid, bob_id, bob_private_key, 92 );
   BOOST_REQUIRE_EQUAL( pid(db).equity( 92 ).value, 20 );

   liquidate( pid, dan_id, dan_private_key );

   // dan was short 4 and took over 9 of bob's 10, so he is left long 5 and four contracts
   // net off. Longs afterwards: bob 1, alice 4, dan 5 == 10, against carol's short 10.
   const auto* dan_pos = position_of( mid, dan_id );
   BOOST_REQUIRE( nullptr != dan_pos );
   BOOST_CHECK_EQUAL( dan_pos->size.value, 5 );
   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 10 );
   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()


BOOST_FIXTURE_TEST_SUITE( futures_settlement_tests, futures_fixture )

/// A dated contract settles once, at a price snapshotted from the oracle, and every position
/// closes against that same number.
BOOST_AUTO_TEST_CASE( a_dated_contract_settles_everyone_at_one_price )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );

   const fc::time_point_sec expiry = db.head_block_time() + fc::days( 1 );
   const auto mid = make_market( alice_id, alice_private_key, oid, 1, expiry, "BTC-DATED" );

   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   const auto bob_pos   = position_of( mid, bob_id )->get_id();
   const auto carol_pos = position_of( mid, carol_id )->get_id();

   // before expiry, settlement is refused
   GRAPHENE_REQUIRE_THROW( settle_market( mid, alice_id, alice_private_key ), fc::exception );

   // move past expiry with the mark at 110: bob is up 100, carol down 100
   generate_blocks( expiry + 60 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 110 );

   const auto bob_before   = db.get_balance( bob_id, core_id ).amount;
   const auto carol_before = db.get_balance( carol_id, core_id ).amount;

   settle_market( mid, alice_id, alice_private_key, bob_pos );
   BOOST_CHECK( mid(db).is_settled );
   BOOST_REQUIRE( mid(db).settlement_price.valid() );
   BOOST_CHECK_EQUAL( mid(db).settlement_price->value, 110 );
   BOOST_CHECK( nullptr == db.find( bob_pos ) );

   // bob: 100 margin plus 100 profit
   BOOST_CHECK_EQUAL( ( db.get_balance( bob_id, core_id ).amount - bob_before ).value, 200 );

   // the price is fixed now, so a later oracle move cannot change what carol settles at
   publish( oid, bob_id, bob_private_key, 500 );
   BOOST_CHECK_EQUAL( mid(db).settlement_price->value, 110 );

   settle_market( mid, alice_id, alice_private_key, carol_pos );
   BOOST_CHECK( nullptr == db.find( carol_pos ) );
   // carol: 100 margin less her 100 loss
   BOOST_CHECK_EQUAL( ( db.get_balance( carol_id, core_id ).amount - carol_before ).value, 0 );

   BOOST_CHECK_EQUAL( mid(db).open_interest.value, 0 );
} FC_LOG_AND_RETHROW() }

/// A settled market takes no further orders, and a perpetual can never be settled at all.
BOOST_AUTO_TEST_CASE( settled_markets_stop_trading_and_perpetuals_never_settle )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );

   const auto perp = make_market( alice_id, alice_private_key, oid, 1, {}, "BTC-PERP" );
   GRAPHENE_REQUIRE_THROW( settle_market( perp, alice_id, alice_private_key ), fc::exception );

   const fc::time_point_sec expiry = db.head_block_time() + fc::days( 1 );
   const auto dated = make_market( alice_id, alice_private_key, oid, 1, expiry, "BTC-DATED" );

   generate_blocks( expiry + 60 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 100 );

   settle_market( dated, alice_id, alice_private_key );
   BOOST_CHECK( dated(db).is_settled );
   BOOST_CHECK( !dated(db).is_tradable( db.head_block_time() ) );
   GRAPHENE_REQUIRE_THROW( place( dated, bob_id, bob_private_key, true, 100, 1 ), fc::exception );

   // the perpetual is unaffected
   BOOST_CHECK( perp(db).is_tradable( db.head_block_time() ) );
} FC_LOG_AND_RETHROW() }

/// Funding moves value from longs to shorts when the book sits above the mark, and nets to
/// zero across the market.
BOOST_AUTO_TEST_CASE( funding_transfers_from_longs_to_shorts_when_the_book_is_above_the_mark )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );

   futures_market_create_operation cop;
   cop.owner            = alice_id;
   cop.symbol           = "BTC-PERP";
   cop.oracle_id        = oid;
   cop.collateral_asset = core_id;
   cop.contract_size    = 1;
   cop.options.funding_interval_sec = 60;      // short, so a test can cross it
   cop.options.max_funding_rate_ppm = 10000;   // 1% per interval
   signed_transaction ctx;
   ctx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( ctx.operations.back() );
   set_expiration( db, ctx );
   ctx.sign( alice_private_key, db.get_chain_id() );
   const futures_market_id_type mid {
      PUSH_TX( db, ctx ).operation_results.front().get<object_id_type>() };

   // bob long 10, carol short 10, both at the mark
   place( mid, bob_id, bob_private_key, true, 100, 10 );
   place( mid, carol_id, carol_private_key, false, 100, 10 );
   const auto bob_pos   = position_of( mid, bob_id )->get_id();
   const auto carol_pos = position_of( mid, carol_id )->get_id();
   BOOST_CHECK_EQUAL( mid(db).cumulative_funding.value, 0 );

   // leave a book strictly above the mark: bid 104, ask 108, mid 106 against a mark of 100
   place( mid, dan_id, dan_private_key, true, 104, 1 );
   place( mid, alice_id, alice_private_key, false, 108, 1 );

   // cross a funding interval and refresh the mark
   generate_blocks( db.head_block_time() + 120 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 100 );

   // premium is 6 per contract, capped at 1% of the 100 mark = 1
   BOOST_CHECK_EQUAL( mid(db).cumulative_funding.value, 1 );

   const auto bob_margin_before   = bob_pos(db).margin;
   const auto carol_margin_before = carol_pos(db).margin;

   // touching each position applies the accrued funding
   adjust_margin( bob_pos, bob_id, bob_private_key, 1000 );
   adjust_margin( carol_pos, carol_id, carol_private_key, 1000 );

   // bob is long 10 and pays 10; carol is short 10 and receives 10
   BOOST_CHECK_EQUAL( ( bob_pos(db).margin - bob_margin_before ).value, 1000 - 10 );
   BOOST_CHECK_EQUAL( ( carol_pos(db).margin - carol_margin_before ).value, 1000 + 10 );

   check_market_is_balanced( mid );
} FC_LOG_AND_RETHROW() }

/// A funding cap below 100 ppm must still work. It used to be converted into
/// GRAPHENE_100_PERCENT units by an integer divide by 100, which turned every rate under
/// 100 ppm into zero and switched funding off without saying so.
BOOST_AUTO_TEST_CASE( a_small_funding_cap_is_not_rounded_away )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dan) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) );
   fund( carol, asset(10000000) ); fund( dan, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100000 );   // a mark big enough for ppm to bite

   futures_market_create_operation cop;
   cop.owner            = alice_id;
   cop.symbol           = "BTC-PERP";
   cop.oracle_id        = oid;
   cop.collateral_asset = core_id;
   cop.contract_size    = 1;
   cop.options.funding_interval_sec = 60;
   cop.options.max_funding_rate_ppm = 50;   // below the old conversion's 100 ppm floor
   signed_transaction ctx;
   ctx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( ctx.operations.back() );
   set_expiration( db, ctx );
   ctx.sign( alice_private_key, db.get_chain_id() );
   const futures_market_id_type mid {
      PUSH_TX( db, ctx ).operation_results.front().get<object_id_type>() };

   // a two-sided book well above the mark, so the premium is large and the cap is what binds
   place( mid, dan_id, dan_private_key, true, 110000, 1 );
   place( mid, alice_id, alice_private_key, false, 120000, 1 );

   generate_blocks( db.head_block_time() + 120 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 100000 );

   // 50 ppm of a 100000 mark is 5, and it must not be zero
   BOOST_CHECK_EQUAL( mid(db).cumulative_funding.value, 5 );
} FC_LOG_AND_RETHROW() }

/// With one side of the book empty there is no mid, so no funding is charged. Guessing a
/// funding rate would move real money on a made-up number.
BOOST_AUTO_TEST_CASE( no_funding_accrues_without_a_two_sided_book )
{ try {
   generate_blocks( HARDFORK_FUTURES_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice, asset(10000000) ); fund( bob, asset(10000000) ); fund( carol, asset(10000000) );

   const auto oid = make_oracle( alice_id, alice_private_key, bob_id );
   publish( oid, bob_id, bob_private_key, 100 );

   futures_market_create_operation cop;
   cop.owner            = alice_id;
   cop.symbol           = "BTC-PERP";
   cop.oracle_id        = oid;
   cop.collateral_asset = core_id;
   cop.contract_size    = 1;
   cop.options.funding_interval_sec = 60;
   signed_transaction ctx;
   ctx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( ctx.operations.back() );
   set_expiration( db, ctx );
   ctx.sign( alice_private_key, db.get_chain_id() );
   const futures_market_id_type mid {
      PUSH_TX( db, ctx ).operation_results.front().get<object_id_type>() };

   // only bids rest
   place( mid, bob_id, bob_private_key, true, 104, 1 );

   generate_blocks( db.head_block_time() + 120 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 100 );

   BOOST_CHECK_EQUAL( mid(db).cumulative_funding.value, 0 );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
