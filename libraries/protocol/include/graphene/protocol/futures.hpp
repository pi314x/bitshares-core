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

#include <graphene/protocol/base.hpp>
#include <graphene/protocol/asset.hpp>

namespace graphene { namespace protocol {

   constexpr size_t GRAPHENE_FUTURES_MIN_SYMBOL_LENGTH = 3;
   constexpr size_t GRAPHENE_FUTURES_MAX_SYMBOL_LENGTH = 32;
   constexpr size_t GRAPHENE_FUTURES_MAX_DESCRIPTION_LENGTH = 1000;

   /// Leverage is bounded by the initial margin ratio; 1% initial margin is 100x, which is
   /// already beyond what most venues offer and well beyond what a thin book can liquidate.
   constexpr uint16_t GRAPHENE_FUTURES_MIN_INITIAL_MARGIN_RATIO = 100;   // 1%, i.e. 100x

   /// A dated contract more than this far out is almost certainly a typo in the timestamp.
   constexpr uint32_t GRAPHENE_FUTURES_MAX_EXPIRY_DAYS = 3650;

   /// Per-interval funding cap. 1% of notional per interval is already extreme.
   constexpr uint32_t GRAPHENE_FUTURES_MAX_FUNDING_RATE_PPM = 10000;

   constexpr uint32_t GRAPHENE_FUTURES_MIN_FUNDING_INTERVAL_SEC = 60;

   /// Owner-settable market policy. Shared by create and update.
   struct futures_market_options
   {
      /**
       * Fraction of notional that must be posted to open, in GRAPHENE_100_PERCENT units.
       * This is what bounds leverage: 1000 (10%) is 10x.
       */
      uint16_t initial_margin_ratio = 1000;

      /**
       * Fraction of notional below which a position is liquidated, in GRAPHENE_100_PERCENT
       * units. Must be below @ref initial_margin_ratio, or a position would be liquidatable
       * the instant it opened.
       */
      uint16_t maintenance_margin_ratio = 500;

      /// Perpetuals only, ignored by dated contracts.
      uint32_t funding_interval_sec = 28800;   // 8 hours, the common venue default
      uint32_t max_funding_rate_ppm = 750;     // 0.075% per interval

      /// Whether new orders are accepted. Lets an owner halt a market without deleting it,
      /// which matters because deleting one with open positions must never be possible.
      bool enabled = true;

      extensions_type extensions;

      void validate()const;
   };

   /**
    * @brief Defines a futures contract.
    * @ingroup operations
    *
    * The price of a contract is quoted as an integer amount of @ref collateral_asset per
    * contract, never as a ratio. See FUTURES-DESIGN.md: it is what makes position accounting
    * exact, and exactness is what keeps the market solvent.
    */
   struct futures_market_create_operation : public base_operation
   {
      struct fee_params_t {
         uint64_t fee = 1000 * GRAPHENE_BLOCKCHAIN_PRECISION;
         uint32_t price_per_kbyte = GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset            fee;
      account_id_type  owner;

      /// Unique, e.g. "BTC-PERP". Uppercase letters, digits, '.' and '-'.
      string           symbol;
      string           description;

      /// Index price source. Its quote asset must be @ref collateral_asset, so that PnL and
      /// margin are denominated in the same thing and no conversion is ever needed.
      oracle_id_type   oracle_id;

      /// Margin and PnL currency.
      asset_id_type    collateral_asset;

      /// Units of the oracle's base asset represented by one contract.
      share_type       contract_size;

      /// Absent means perpetual. Present means the contract settles at the oracle price at
      /// this time and then accepts nothing further.
      optional<time_point_sec> expiry;

      futures_market_options options;

      extensions_type  extensions;

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee( const fee_params_t& k )const;
   };

   /**
    * @brief Changes a futures market's policy.
    * @ingroup operations
    *
    * The oracle, collateral asset, contract size and expiry cannot be changed. Traders hold
    * positions priced against all four, and altering any of them underneath an open position
    * would silently change what that position is worth.
    */
   struct futures_market_update_operation : public base_operation
   {
      struct fee_params_t {
         uint64_t fee = 50 * GRAPHENE_BLOCKCHAIN_PRECISION;
         uint32_t price_per_kbyte = GRAPHENE_BLOCKCHAIN_PRECISION;
      };

      asset                    fee;
      account_id_type          owner;
      futures_market_id_type   market_id;

      optional<string>                 new_description;
      optional<futures_market_options> new_options;

      extensions_type          extensions;

      account_id_type fee_payer()const { return owner; }
      void            validate()const;
      share_type      calculate_fee( const fee_params_t& k )const;
   };

   /// @return whether @p symbol is acceptable as a futures market symbol
   bool is_valid_futures_symbol( const string& symbol );

} } // graphene::protocol

FC_REFLECT( graphene::protocol::futures_market_options,
            (initial_margin_ratio)(maintenance_margin_ratio)
            (funding_interval_sec)(max_funding_rate_ppm)(enabled)(extensions) )

FC_REFLECT( graphene::protocol::futures_market_create_operation::fee_params_t,
            (fee)(price_per_kbyte) )
FC_REFLECT( graphene::protocol::futures_market_update_operation::fee_params_t,
            (fee)(price_per_kbyte) )

FC_REFLECT( graphene::protocol::futures_market_create_operation,
            (fee)(owner)(symbol)(description)(oracle_id)(collateral_asset)(contract_size)
            (expiry)(options)(extensions) )
FC_REFLECT( graphene::protocol::futures_market_update_operation,
            (fee)(owner)(market_id)(new_description)(new_options)(extensions) )

GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::futures_market_options )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION(
      graphene::protocol::futures_market_create_operation::fee_params_t )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION(
      graphene::protocol::futures_market_update_operation::fee_params_t )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::futures_market_create_operation )
GRAPHENE_DECLARE_EXTERNAL_SERIALIZATION( graphene::protocol::futures_market_update_operation )
