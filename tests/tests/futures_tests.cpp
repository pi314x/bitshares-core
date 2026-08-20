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
