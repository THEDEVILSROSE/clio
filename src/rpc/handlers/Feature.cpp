//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2024, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include "rpc/handlers/Feature.hpp"

#include "data/Types.hpp"
#include "rpc/Errors.hpp"
#include "rpc/JS.hpp"
#include "rpc/RPCHelpers.hpp"
#include "rpc/common/MetaProcessors.hpp"
#include "rpc/common/Specs.hpp"
#include "rpc/common/Types.hpp"
#include "rpc/common/Validators.hpp"

#include <boost/json/conversion.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/LedgerHeader.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rpc {

FeatureHandler::Result
FeatureHandler::process(FeatureHandler::Input input, Context const& ctx) const
{
    namespace vs = std::views;
    namespace rg = std::ranges;

    auto const range = sharedPtrBackend_->fetchLedgerRange();
    auto const lgrInfoOrStatus = getLedgerHeaderFromHashOrSeq(
        *sharedPtrBackend_, ctx.yield, input.ledgerHash, input.ledgerIndex, range->maxSequence
    );

    if (auto const status = std::get_if<Status>(&lgrInfoOrStatus))
        return Error{*status};

    auto const lgrInfo = std::get<ripple::LedgerHeader>(lgrInfoOrStatus);
    auto const& all = amendmentCenter_->getAll();

    auto searchPredicate = [search = input.feature](auto const& feature) {
        if (search)
            return ripple::to_string(feature.feature) == search.value() or feature.name == search.value();
        return true;
    };

    std::vector<Output::Feature> filtered;
    rg::transform(all | vs::filter(searchPredicate), std::back_inserter(filtered), [&](auto const& feature) {
        return Output::Feature{
            .name = feature.name,
            .key = ripple::to_string(feature.feature),
            .supported = feature.isSupportedByClio,
        };
    });

    if (filtered.empty())
        return Error{Status{RippledError::rpcBAD_FEATURE}};

    std::vector<data::AmendmentKey> names;
    rg::transform(filtered, std::back_inserter(names), [](auto const& feature) { return feature.name; });

    std::map<std::string, Output::Feature> features;
    rg::transform(
        filtered,
        amendmentCenter_->isEnabled(ctx.yield, names, lgrInfo.seq),
        std::inserter(features, std::end(features)),
        [&](Output::Feature feature, bool isEnabled) {
            feature.enabled = isEnabled;
            return std::make_pair(feature.key, std::move(feature));
        }
    );

    return Output{
        .features = std::move(features),
        .ledgerHash = ripple::strHex(lgrInfo.hash),
        .ledgerIndex = lgrInfo.seq,
        .inlineResult = input.feature.has_value()
    };
}

RpcSpecConstRef
FeatureHandler::spec([[maybe_unused]] uint32_t apiVersion)
{
    static RpcSpec const rpcSpec = {
        {JS(feature), validation::Type<std::string>{}},
        {JS(vetoed),
         meta::WithCustomError{
             validation::NotSupported{},
             Status(RippledError::rpcNO_PERMISSION, "The admin portion of feature API is not available through Clio.")
         }},
        {JS(ledger_hash), validation::CustomValidators::Uint256HexStringValidator},
        {JS(ledger_index), validation::CustomValidators::LedgerIndexValidator},
    };
    return rpcSpec;
}

void
tag_invoke(boost::json::value_from_tag, boost::json::value& jv, FeatureHandler::Output const& output)
{
    using boost::json::value_from;

    if (output.inlineResult) {
        jv = value_from(output.features);
    } else {
        jv = {
            {JS(features), value_from(output.features)},
        };
    }

    auto& obj = jv.as_object();
    obj[JS(ledger_hash)] = output.ledgerHash;
    obj[JS(ledger_index)] = output.ledgerIndex;
    obj[JS(validated)] = output.validated;
}

void
tag_invoke(boost::json::value_from_tag, boost::json::value& jv, FeatureHandler::Output::Feature const& feature)
{
    using boost::json::value_from;

    jv = {
        {JS(name), feature.name},
        {JS(enabled), feature.enabled},
        {JS(supported), feature.supported},
    };
}

FeatureHandler::Input
tag_invoke(boost::json::value_to_tag<FeatureHandler::Input>, boost::json::value const& jv)
{
    auto input = FeatureHandler::Input{};
    auto const jsonObject = jv.as_object();

    if (jsonObject.contains(JS(feature)))
        input.feature = jv.at(JS(feature)).as_string();

    if (jsonObject.contains(JS(ledger_hash)))
        input.ledgerHash = boost::json::value_to<std::string>(jv.at(JS(ledger_hash)));

    if (jsonObject.contains(JS(ledger_index))) {
        if (!jsonObject.at(JS(ledger_index)).is_string()) {
            input.ledgerIndex = jv.at(JS(ledger_index)).as_int64();
        } else if (jsonObject.at(JS(ledger_index)).as_string() != "validated") {
            input.ledgerIndex = std::stoi(boost::json::value_to<std::string>(jv.at(JS(ledger_index))));
        }
    }
    return input;
}

}  // namespace rpc
