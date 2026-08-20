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
#include <graphene/chain/evaluator.hpp>

#include <graphene/protocol/futures.hpp>

namespace graphene { namespace chain {

   class futures_market_object;
   class oracle_object;

   /**
    * Recomputes a market's mark price from its oracle.
    *
    * The oracle publishes a ratio; a futures market quotes an integer amount of collateral per
    * contract. This is the one place in the whole futures design where a division happens, and
    * it reuses asset::operator* rather than introducing a second rounding convention.
    *
    * An oracle with no value leaves the mark absent, which halts trading rather than letting
    * risk be assessed against a dead price.
    */
   void update_futures_mark_price( database& d, const futures_market_object& market );

   /// Pushes a new oracle value out to every futures market that uses it.
   void update_futures_markets_for_oracle( database& d, const oracle_object& o );

   class futures_market_create_evaluator : public evaluator<futures_market_create_evaluator>
   {
      public:
         using operation_type = futures_market_create_operation;

         void_result    do_evaluate( const futures_market_create_operation& op ) const;
         object_id_type do_apply( const futures_market_create_operation& op ) const;
   };

   class futures_market_update_evaluator : public evaluator<futures_market_update_evaluator>
   {
      public:
         using operation_type = futures_market_update_operation;

         void_result do_evaluate( const futures_market_update_operation& op );
         void_result do_apply( const futures_market_update_operation& op ) const;

         const futures_market_object* _market = nullptr;
   };

} } // graphene::chain
