/*
 * Oracles: named price series independent of any asset.
 *
 * The aggregation tests below are the ones that matter. An oracle's whole purpose is to be the
 * number other things trust, so the interesting cases are not "does a value round-trip" but
 * "what happens when producers disagree, go quiet, or all move at once".
 */

#include <boost/test/unit_test.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/oracle_object.hpp>
#include <graphene/protocol/oracle.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_AUTO_TEST_SUITE( oracle_name_tests )

BOOST_AUTO_TEST_CASE( valid_and_invalid_oracle_names )
{
   BOOST_CHECK( is_valid_oracle_name( "BTC.USD" ) );
   BOOST_CHECK( is_valid_oracle_name( "BTS" ) );
   BOOST_CHECK( is_valid_oracle_name( "EUR-USD" ) );
   BOOST_CHECK( is_valid_oracle_name( "GOLD.XAU.2026" ) );

   BOOST_CHECK( !is_valid_oracle_name( "AB" ) );            // too short
   BOOST_CHECK( !is_valid_oracle_name( std::string( 33, 'A' ) ) ); // too long
   // Lowercase is rejected so that "btc.usd" cannot sit alongside "BTC.USD" as a lookalike.
   BOOST_CHECK( !is_valid_oracle_name( "btc.usd" ) );
   BOOST_CHECK( !is_valid_oracle_name( "BTC USD" ) );       // space
   BOOST_CHECK( !is_valid_oracle_name( "BTC_USD" ) );       // underscore
   BOOST_CHECK( !is_valid_oracle_name( ".BTCUSD" ) );       // leading separator
   BOOST_CHECK( !is_valid_oracle_name( "BTCUSD." ) );       // trailing separator
   BOOST_CHECK( !is_valid_oracle_name( "BTC..USD" ) );      // repeated separator
   BOOST_CHECK( !is_valid_oracle_name( "BTC.-USD" ) );      // mixed repeated separator
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE( oracle_tests, database_fixture )

namespace {

/// Prices are compared by cross-multiplication, so two prices that reduce to the same ratio
/// are equal even when written with different amounts. Tests state prices in whole units and
/// rely on that rather than on a particular representation.
price px( int64_t base_amount, asset_id_type base_id, int64_t quote_amount, asset_id_type quote_id )
{
   return price( asset( base_amount, base_id ), asset( quote_amount, quote_id ) );
}

} // namespace

/// Every oracle operation must be refused before the hardfork, or a node running this code
/// could produce a block that a node running the previous release cannot validate.
BOOST_AUTO_TEST_CASE( oracle_ops_are_refused_before_the_hardfork )
{ try {
   ACTORS( (alice) );
   fund( alice );

   oracle_create_operation op;
   op.owner       = alice_id;
   op.name        = "BTC.USD";
   op.base_asset  = asset_id_type();
   op.quote_asset = asset_id_type();      // replaced below
   op.options.producers[alice_id] = 1;

   const asset_object& usd = create_user_issued_asset( "USDTEST" );
   op.quote_asset = usd.get_id();

   signed_transaction tx;
   tx.operations.push_back( op );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, tx ), fc::exception );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( create_publish_and_aggregate )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );

   ACTORS( (alice)(bob)(carol)(dave) );
   fund( alice );
   fund( bob );
   fund( carol );
   fund( dave );

   const asset_object& usd = create_user_issued_asset( "USDTEST" );
   const auto core = asset_id_type();
   const auto usd_id = usd.get_id();

   // three producers, quorum of two
   oracle_create_operation cop;
   cop.owner       = alice_id;
   cop.name        = "CORE.USD";
   cop.description = "core against a test dollar";
   cop.base_asset  = core;
   cop.quote_asset = usd_id;
   cop.options.producers[bob_id]   = 1;
   cop.options.producers[carol_id] = 1;
   cop.options.producers[dave_id]  = 1;
   cop.options.minimum_producers   = 2;
   cop.options.value_lifetime_sec  = 3600;

   signed_transaction tx;
   tx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   auto ptx = PUSH_TX( db, tx );
   const oracle_id_type oid { ptx.operation_results.front().get<object_id_type>() };

   // a fresh oracle asserts nothing
   BOOST_CHECK( !oid(db).current_value.valid() );

   auto publish = [&]( account_id_type who, const fc::ecc::private_key& key, int64_t quote ) {
      oracle_publish_operation pop;
      pop.producer  = who;
      pop.oracle_id = oid;
      pop.value     = px( 1, core, quote, usd_id );
      signed_transaction ptx2;
      ptx2.operations.push_back( pop );
      db.current_fee_schedule().set_fee( ptx2.operations.back() );
      set_expiration( db, ptx2 );
      ptx2.sign( key, db.get_chain_id() );
      PUSH_TX( db, ptx2 );
   };

   // one submission is below quorum, so still no value -- not a value derived from one source
   publish( bob_id, bob_private_key, 100 );
   BOOST_CHECK( !oid(db).current_value.valid() );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 0u );

   // two submissions reach quorum; lower median of {100, 200} is 100
   publish( carol_id, carol_private_key, 200 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == px( 1, core, 100, usd_id ) );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 2u );

   // three submissions: median of {100, 200, 300} is 200
   publish( dave_id, dave_private_key, 300 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == px( 1, core, 200, usd_id ) );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 3u );
} FC_LOG_AND_RETHROW() }

/// A non-producer must not be able to move the number, whatever else it can do on chain.
BOOST_AUTO_TEST_CASE( a_non_producer_cannot_publish )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );

   ACTORS( (alice)(bob)(mallory) );
   fund( alice );
   fund( bob );
   fund( mallory );

   const asset_object& usd = create_user_issued_asset( "USDTEST" );
   const auto core = asset_id_type();
   const auto usd_id = usd.get_id();

   oracle_create_operation cop;
   cop.owner       = alice_id;
   cop.name        = "CORE.USD";
   cop.base_asset  = core;
   cop.quote_asset = usd_id;
   cop.options.producers[bob_id] = 1;
   cop.options.minimum_producers = 1;

   signed_transaction tx;
   tx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   const oracle_id_type oid { PUSH_TX( db, tx ).operation_results.front().get<object_id_type>() };

   oracle_publish_operation pop;
   pop.producer  = mallory_id;
   pop.oracle_id = oid;
   pop.value     = px( 1, core, 1, usd_id );
   signed_transaction bad;
   bad.operations.push_back( pop );
   db.current_fee_schedule().set_fee( bad.operations.back() );
   set_expiration( db, bad );
   bad.sign( mallory_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, bad ), fc::exception );

   // and the owner is not implicitly a producer either
   pop.producer = alice_id;
   signed_transaction owner_tx;
   owner_tx.operations.push_back( pop );
   db.current_fee_schedule().set_fee( owner_tx.operations.back() );
   set_expiration( db, owner_tx );
   owner_tx.sign( alice_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, owner_tx ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// A value in the wrong orientation must be refused rather than silently inverted: a consumer
/// must never have to work out which way round a number is.
BOOST_AUTO_TEST_CASE( a_value_in_the_wrong_orientation_is_refused )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );

   ACTORS( (alice)(bob) );
   fund( alice );
   fund( bob );

   const asset_object& usd = create_user_issued_asset( "USDTEST" );
   const auto core = asset_id_type();
   const auto usd_id = usd.get_id();

   oracle_create_operation cop;
   cop.owner       = alice_id;
   cop.name        = "CORE.USD";
   cop.base_asset  = core;
   cop.quote_asset = usd_id;
   cop.options.producers[bob_id] = 1;
   cop.options.minimum_producers = 1;

   signed_transaction tx;
   tx.operations.push_back( cop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( alice_private_key, db.get_chain_id() );
   const oracle_id_type oid { PUSH_TX( db, tx ).operation_results.front().get<object_id_type>() };

   oracle_publish_operation pop;
   pop.producer  = bob_id;
   pop.oracle_id = oid;
   pop.value     = px( 1, usd_id, 100, core );   // inverted
   signed_transaction bad;
   bad.operations.push_back( pop );
   db.current_fee_schedule().set_fee( bad.operations.back() );
   set_expiration( db, bad );
   bad.sign( bob_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, bad ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// Names identify oracles to humans, so a duplicate is how a consumer gets pointed at the
/// wrong series.
BOOST_AUTO_TEST_CASE( oracle_names_are_unique )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );

   ACTORS( (alice)(bob) );
   fund( alice );
   fund( bob );

   const asset_object& usd = create_user_issued_asset( "USDTEST" );

   auto make = [&]( account_id_type owner, const fc::ecc::private_key& key ) {
      oracle_create_operation cop;
      cop.owner       = owner;
      cop.name        = "CORE.USD";
      cop.base_asset  = asset_id_type();
      cop.quote_asset = usd.get_id();
      cop.options.producers[owner] = 1;
      cop.options.minimum_producers = 1;
      signed_transaction tx;
      tx.operations.push_back( cop );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      return PUSH_TX( db, tx );
   };

   make( alice_id, alice_private_key );
   // a different owner does not get to reuse the name
   GRAPHENE_REQUIRE_THROW( make( bob_id, bob_private_key ), fc::exception );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

/// Builds and pushes an oracle_create, returning the new id. Factored out because most of the
/// interesting tests differ only in the options they set.
struct oracle_fixture : database_fixture
{
   asset_id_type core_id;
   asset_id_type usd_id;

   void setup_assets()
   {
      core_id = asset_id_type();
      usd_id  = create_user_issued_asset( "USDTEST" ).get_id();
   }

   /// price = base/quote, so px(1, core, N, usd) reads "1 CORE is worth N USD". Note that a
   /// *larger* N is a numerically *smaller* price in this orientation, since the ratio is 1/N.
   /// Ordering, and therefore the median, is by that ratio.
   price usd_per_core( int64_t n ) const
   {
      return price( asset( 1, core_id ), asset( n, usd_id ) );
   }

   oracle_id_type make_oracle( account_id_type owner, const fc::ecc::private_key& key,
                               const string& name, const oracle_options& opts )
   {
      oracle_create_operation op;
      op.owner       = owner;
      op.name        = name;
      op.base_asset  = core_id;
      op.quote_asset = usd_id;
      op.options     = opts;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      return oracle_id_type { PUSH_TX( db, tx ).operation_results.front().get<object_id_type>() };
   }

   void publish( oracle_id_type oid, account_id_type who, const fc::ecc::private_key& key,
                 int64_t n )
   {
      oracle_publish_operation op;
      op.producer  = who;
      op.oracle_id = oid;
      op.value     = usd_per_core( n );
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }

   void update_options( oracle_id_type oid, account_id_type owner,
                        const fc::ecc::private_key& key, const oracle_options& opts )
   {
      oracle_update_operation op;
      op.owner       = owner;
      op.oracle_id   = oid;
      op.new_options = opts;
      signed_transaction tx;
      tx.operations.push_back( op );
      db.current_fee_schedule().set_fee( tx.operations.back() );
      set_expiration( db, tx );
      tx.sign( key, db.get_chain_id() );
      PUSH_TX( db, tx );
   }
};

BOOST_FIXTURE_TEST_SUITE( oracle_behaviour_tests, oracle_fixture )

/// A submission that has aged past value_lifetime_sec must stop counting. An oracle that kept
/// quoting a price nobody is still asserting is worse than one that reports nothing.
BOOST_AUTO_TEST_CASE( a_stale_submission_stops_counting )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice ); fund( bob ); fund( carol );

   oracle_options opts;
   opts.producers[bob_id]   = 1;
   opts.producers[carol_id] = 1;
   opts.minimum_producers   = 2;
   opts.value_lifetime_sec  = 600;
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   publish( oid, bob_id,   bob_private_key,   100 );
   publish( oid, carol_id, carol_private_key, 100 );
   BOOST_REQUIRE( oid(db).current_value.valid() );

   // let both submissions age out, then have one producer republish
   generate_blocks( db.head_block_time() + 601 );
   set_expiration( db, trx );
   publish( oid, bob_id, bob_private_key, 100 );

   // carol's value is stale, so only one live submission remains -- below quorum
   BOOST_CHECK( !oid(db).current_value.valid() );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 0u );

   // carol republishing restores quorum
   publish( oid, carol_id, carol_private_key, 100 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );
} FC_LOG_AND_RETHROW() }

/// Weight is the difference between an oracle and a plain vote count.
BOOST_AUTO_TEST_CASE( weights_shift_the_median )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dave) );
   fund( alice ); fund( bob ); fund( carol ); fund( dave );

   // bob outweighs the other two combined, so his value is the median whatever they say
   oracle_options opts;
   opts.producers[bob_id]   = 10;
   opts.producers[carol_id] = 1;
   opts.producers[dave_id]  = 1;
   opts.minimum_producers   = 2;
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   publish( oid, bob_id,   bob_private_key,   100 );
   publish( oid, carol_id, carol_private_key, 300 );
   publish( oid, dave_id,  dave_private_key,  400 );

   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );

   // with equal weights the same three submissions give the middle value instead
   oracle_options equal = opts;
   equal.producers[bob_id] = 1;
   update_options( oid, alice_id, alice_private_key, equal );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 300 ) );
} FC_LOG_AND_RETHROW() }

/// A minority disagreeing with the rest is filtered out.
BOOST_AUTO_TEST_CASE( an_outlier_is_excluded_while_quorum_survives )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dave) );
   fund( alice ); fund( bob ); fund( carol ); fund( dave );

   oracle_options opts;
   opts.producers[bob_id]   = 1;
   opts.producers[carol_id] = 1;
   opts.producers[dave_id]  = 1;
   opts.minimum_producers   = 2;
   opts.max_deviation_ppm   = 100000;   // 10%
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   // establish a baseline of 100
   publish( oid, bob_id,   bob_private_key,   100 );
   publish( oid, carol_id, carol_private_key, 100 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );

   // dave publishes wildly off; two honest submissions still meet quorum, so he is dropped
   publish( oid, dave_id, dave_private_key, 100000 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 2u );
} FC_LOG_AND_RETHROW() }

/// ...but when everyone moves together, that is a real crash and the oracle must follow it.
/// Freezing here is the failure mode the quorum-aware rule exists to prevent.
BOOST_AUTO_TEST_CASE( when_every_producer_moves_together_the_oracle_moves )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol) );
   fund( alice ); fund( bob ); fund( carol );

   oracle_options opts;
   opts.producers[bob_id]   = 1;
   opts.producers[carol_id] = 1;
   opts.minimum_producers   = 2;
   opts.max_deviation_ppm   = 100000;   // 10%
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   publish( oid, bob_id,   bob_private_key,   100 );
   publish( oid, carol_id, carol_private_key, 100 );
   BOOST_REQUIRE( oid(db).current_value.valid() );

   // both producers report a 10x move, far outside the deviation bound. Excluding them would
   // leave zero submissions and break quorum, so they are believed.
   publish( oid, bob_id,   bob_private_key,   1000 );
   publish( oid, carol_id, carol_private_key, 1000 );

   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 1000 ) );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 2u );
} FC_LOG_AND_RETHROW() }

/// Removing a producer is what an owner does when that producer misbehaves, so it has to bite
/// straight away rather than when its last submission happens to expire.
BOOST_AUTO_TEST_CASE( removing_a_producer_takes_effect_immediately )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob)(carol)(dave) );
   fund( alice ); fund( bob ); fund( carol ); fund( dave );

   oracle_options opts;
   opts.producers[bob_id]   = 1;
   opts.producers[carol_id] = 1;
   opts.producers[dave_id]  = 1;
   opts.minimum_producers   = 2;
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   publish( oid, bob_id,   bob_private_key,   100 );
   publish( oid, carol_id, carol_private_key, 200 );
   publish( oid, dave_id,  dave_private_key,  300 );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 200 ) );

   // drop dave; his stored submission must stop counting at once
   oracle_options without_dave;
   without_dave.producers[bob_id]   = 1;
   without_dave.producers[carol_id] = 1;
   without_dave.minimum_producers   = 2;
   update_options( oid, alice_id, alice_private_key, without_dave );

   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK_EQUAL( oid(db).current_value_producer_count, 2u );
   // median of {100, 200} with the upper-median convention
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );

   // and dave can no longer publish at all
   GRAPHENE_REQUIRE_THROW( publish( oid, dave_id, dave_private_key, 500 ), fc::exception );
} FC_LOG_AND_RETHROW() }

/// A windowed median must not be movable by one block's worth of prices.
BOOST_AUTO_TEST_CASE( a_windowed_median_resists_a_single_spike )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   oracle_options opts;
   opts.producers[bob_id]  = 1;
   opts.minimum_producers  = 1;
   opts.value_lifetime_sec = 86400;
   opts.aggregation        = oracle_aggregation_method::median_over_window;
   opts.window_sec         = 86400;
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   // a settled history at 100
   for( int i = 0; i < 5; ++i )
   {
      publish( oid, bob_id, bob_private_key, 100 );
      generate_block();
      set_expiration( db, trx );
   }
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );

   // one spike does not move the windowed median, even though it is the latest value
   publish( oid, bob_id, bob_private_key, 100000 );
   BOOST_REQUIRE( oid(db).current_value.valid() );
   BOOST_CHECK( *oid(db).current_value == usd_per_core( 100 ) );
   // the spike is recorded though: the window holds it, it is simply outvoted
   BOOST_CHECK( oid(db).history.back().second == usd_per_core( 100000 ) );
} FC_LOG_AND_RETHROW() }

/// Policy is the owner's alone. A producer that could edit the producer set could appoint
/// itself a majority.
BOOST_AUTO_TEST_CASE( only_the_owner_may_update_or_delete )
{ try {
   generate_blocks( HARDFORK_ORACLE_TIME );
   generate_block();
   set_expiration( db, trx );
   setup_assets();

   ACTORS( (alice)(bob) );
   fund( alice ); fund( bob );

   oracle_options opts;
   opts.producers[bob_id] = 1;
   opts.minimum_producers = 1;
   const auto oid = make_oracle( alice_id, alice_private_key, "CORE.USD", opts );

   oracle_options grab = opts;
   grab.producers[bob_id] = 1000;
   GRAPHENE_REQUIRE_THROW( update_options( oid, bob_id, bob_private_key, grab ), fc::exception );

   oracle_delete_operation dop;
   dop.owner     = bob_id;
   dop.oracle_id = oid;
   signed_transaction tx;
   tx.operations.push_back( dop );
   db.current_fee_schedule().set_fee( tx.operations.back() );
   set_expiration( db, tx );
   tx.sign( bob_private_key, db.get_chain_id() );
   GRAPHENE_REQUIRE_THROW( PUSH_TX( db, tx ), fc::exception );

   // the owner can
   dop.owner = alice_id;
   signed_transaction ok;
   ok.operations.push_back( dop );
   db.current_fee_schedule().set_fee( ok.operations.back() );
   set_expiration( db, ok );
   ok.sign( alice_private_key, db.get_chain_id() );
   PUSH_TX( db, ok );
   BOOST_CHECK( nullptr == db.find( oid ) );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
