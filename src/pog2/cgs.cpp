// Copyright (c) 2018 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pog2/cgs.h"
#include "addressindex.h"
#include "validation.h"
#include "referrals.h"

#include <stack>
#include <deque>
#include <stack>
#include <numeric>

#include <boost/multiprecision/cpp_int.hpp> 

namespace pog2
{
    namespace
    {
        const int NO_GENESIS = 13500;
    }

    using UnspentPair = std::pair<CAddressUnspentKey, CAddressUnspentValue>;

    using BigInt = boost::multiprecision::cpp_int;

    double Age(int height, int tip_height, double maturity)
    {
        assert(tip_height >= 0);
        assert(height <= tip_height);
        assert(maturity > 0);

        const double maturity_scale = maturity / 4.0; //matures to about 97% at 4
        const double age = (tip_height - height) / maturity_scale;

        assert(age >= 0);
        return age;
    }

    double AgeScale(int height, int tip_height, double maturity)
    {
        assert(tip_height >= 0);
        assert(height <= tip_height);
        assert(maturity > 0);

        const auto age = Age(height, tip_height, maturity);
        const auto age_scale =  1.0 - (1.0 / (std::pow(age, 2) + 1.0));

        assert(age_scale >= 0);
        assert(age_scale <= 1.001);
        return age_scale;
    }

    double AgeScale(const Coin& c, int tip_height, int maturity) {
        assert(tip_height >= 0);
        assert(c.height <= tip_height);
        assert(maturity > 0);
        return AgeScale(c.height, tip_height, maturity);
    }

    int GetReferralHeight(
            referral::ReferralsViewCache& db,
            const referral::Address& a
            )
    {
        auto height = db.GetReferralHeight(a);
        if (height < 0) {
            const auto beacon = db.GetReferral(a);
            assert(beacon);

            uint256 hashBlock;
            CBlockIndex* pindex = nullptr;

            referral::ReferralRef beacon_out;
            if (GetReferral(beacon->GetHash(), beacon_out, hashBlock, pindex)) {
                assert(pindex);
                height = pindex->nHeight;
                if (height > 0) {
                    db.SetReferralHeight(height, a);
                }
            }
        }
        return height;
    }

    Coins GetCoins(
            int height,
            char address_type,
            const referral::Address& address) {
        Coins cs;
        std::vector<UnspentPair> unspent;
        if (!GetAddressUnspent(address, address_type, false, unspent)) {
            return cs;
        }

        cs.reserve(unspent.size());
        for (const auto& p : unspent) {
            if (p.first.type == 0 || p.first.isInvite) {
                continue;
            }
            assert(p.second.satoshis >= 0);

            cs.push_back({std::min(p.second.blockHeight, height), p.second.satoshis});
        }

        return cs;
    }

    bool GetAllCoins(int tip_height, AddressCoins& coins) {
        std::vector<UnspentPair> unspent;
        if (!GetAllUnspent(false, [&coins, tip_height](const CAddressUnspentKey& key, const CAddressUnspentValue& value) {
                if (key.type == 0 || key.isInvite || value.satoshis == 0 || value.blockHeight > tip_height) {
                    return;
                }

                assert(!key.isInvite);
                assert(value.satoshis > 0);

                coins[key.hashBytes].emplace_back(value.blockHeight, value.satoshis);
           })) {
            return false;
        }
        return true;
    }

    BalancePair BalanceDecay(int tip_height, const Coin& c, int maturity) {
        assert(tip_height >= 0);
        assert(c.height <= tip_height);
        assert(c.amount >= 0);
        assert(maturity > 0);

        const auto age_scale = AgeScale(c, tip_height, maturity);
        const auto aged_balance = age_scale * c.amount; 

        assert(aged_balance <= std::numeric_limits<CAmount>::max());
        
        CAmount amount = aged_balance;

        assert(amount >= 0);
        assert(amount <= c.amount);
        return BalancePair{amount, c.amount};
    }

    template <class AgeFunc>
    BalancePair AgedBalance(int tip_height, const Coins& cs, int maturity, AgeFunc AgedBalanceFunc) {
        assert(tip_height >= 0);

        BalancePairs balances(cs.size());
        std::transform(cs.begin(), cs.end(), balances.begin(),
                [tip_height, maturity, &AgedBalanceFunc](const Coin& c) {
                    return AgedBalanceFunc(tip_height, c, maturity);
                });

        const auto aged_balance = 
            std::accumulate(balances.begin(), balances.end(), CAmount{0}, 
                [](CAmount amount, const BalancePair& b) {
                    return amount + b.first;
               });

        const auto balance =
            std::accumulate(balances.begin(), balances.end(), CAmount{0}, 
                [](CAmount amount, const BalancePair& b) {
                    return amount + b.second;
                });
        
        assert(aged_balance <= balance);
        return BalancePair{aged_balance, balance};
    }

    BalancePair GetAgedBalance(
            CGSContext& context,
            char address_type,
            const referral::Address& address)
    {
        assert(!context.coins.empty());

        const auto cached_balance = context.balances.find(address);

        if (cached_balance == context.balances.end()) {
            const auto balance = AgedBalance(
                    context.tip_height,
                    context.coins[address],
                    context.coin_maturity,
                    BalanceDecay);

            context.balances[address] = balance;
            return balance;
        }
        return cached_balance->second;
    }

    //Convex function with property that if C0 > C1 and you have A within [0, 1] then 
    //ConvexF(C0 + A) - ConvexF(C0) > ConvexF(C1 + A) - ConvexF(C1);
    //See: Lottery Trees: Motivational Deployment of Networked Systems
    //These properties are important to allow for some kind of growth incentive
    //without compromising the system's integrity against sybil attacks.
    template <class V>
    V ConvexF(V c, V B, V S) { 
        assert(c >= V{0});
        assert(c <= V{1.01});
        assert(B >= V{0});
        assert(B <= V{1.01});
        assert(S >= V{0});
        assert(S <= V{1.01});

        const V v = (B*c) + ((V{1} - B)*boost::multiprecision::pow(c, V{1} + S));
        assert(v >= 0);
        return v;
    }

    Contribution ContributionNode(
            CGSContext& context,
            char address_type,
            const referral::Address& address,
            referral::ReferralsViewCache& db)
    {
        assert(context.tip_height > 0);
        assert(context.new_coin_maturity > 0);

        const auto n = context.contribution.find(address);
        if (n != context.contribution.end()) {
            return n->second;
        }

        const auto aged_balance = GetAgedBalance(
                context,
                address_type,
                address);

        const auto beacon_height = 
            std::min(GetReferralHeight(db, address), context.tip_height);

        if (beacon_height < 0 ) {
            return Contribution{};
        }

        assert(beacon_height <= context.tip_height);

        const auto beacon_age_scale =
            1.0 - AgeScale(beacon_height, context.tip_height, context.new_coin_maturity);

        assert(beacon_age_scale >= 0);
        assert(beacon_age_scale <= 1.01);

        Contribution c;

        //We compute both the linear and sublinear versions of the contribution.
        //This is done because there are two pools of selections evenly split
        //between stake oriented and growth oriented engagements.
        c.value = (beacon_age_scale * aged_balance.second) + aged_balance.first;
        c.sub = boost::multiprecision::log(ContributionAmount{1.0} + c.value);

        assert(c.value >= 0);
        assert(c.value <= aged_balance.second);
        assert(c.sub >= 0);

        context.contribution[address] = c;
        return c;
    }

    using Children = std::vector<referral::Address>;
    struct Node 
    {
        char address_type;
        referral::Address address;
        Children children;
        SubtreeContribution contribution;
    };
    using Nodes = std::stack<Node>;

    /**
     * Computes the subtree contribution rooted at the address specified.
     * This algorithm computes the subtree contribution by doing a post order
     * traversal of the ambassador tree.
     */
    SubtreeContribution ContributionSubtreeIter(
            CGSContext& context,
            char address_type,
            const referral::Address& address,
            referral::ReferralsViewCache& db)
    {
        const auto c = context.subtree_contribution.find(address);

        if (c != context.subtree_contribution.end()) {
            return c->second;
        }

        const auto children = db.GetChildren(address);
        const auto maybe_ref = db.GetReferral(address);

        if (!maybe_ref) {
            return {};
        }

        SubtreeContribution contribution;

        Nodes ns;
        ns.push({
                maybe_ref->addressType,
                maybe_ref->GetAddress(),
                children,
                {}});

        while (!ns.empty()) {
            auto& n = ns.top();
            n.contribution.value += contribution.value;
            n.contribution.sub += contribution.sub;
            n.contribution.tree_size += contribution.tree_size;

            if (n.children.empty()) {
                auto c = ContributionNode(
                        context,
                        n.address_type,
                        n.address,
                        db);

                n.contribution.value += c.value;
                n.contribution.sub += c.sub;
                n.contribution.tree_size++;

                assert(n.contribution.value >= 0);
                assert(n.contribution.sub >= 0);

                contribution = n.contribution;
                context.subtree_contribution[n.address] = n.contribution;

                ns.pop();

            } else {
                const auto child_address = n.children.back();
                n.children.pop_back();

                contribution.value = 0;
                contribution.sub = 0;
                contribution.tree_size = 0;

                const auto children = db.GetChildren(child_address);
                const auto maybe_ref = db.GetReferral(child_address);

                if (maybe_ref) {
                    ns.push({
                            maybe_ref->addressType,
                            maybe_ref->GetAddress(),
                            children,
                            {}});
                }
            }
        }

        return context.subtree_contribution[address];
    }

    ContributionAmount GetValue(const SubtreeContribution& t)
    {
        return t.value;
    }

    ContributionAmount GetSubValue(const SubtreeContribution& t)
    {
        return t.sub;
    }

    struct WeightedScores
    {
        ContributionAmount value;
        ContributionAmount sub;
        size_t tree_size;
    };

    WeightedScores WeightedScore(
            CGSContext& context,
            const char address_type,
            const referral::Address& address,
            referral::ReferralsViewCache& db)
    {
        assert(context.tree_contribution.value > 0);
        assert(context.tree_contribution.sub > 0);

        const auto subtree_contribution =
                ContributionSubtreeIter(
                    context,
                    address_type,
                    address,
                    db);
        assert(subtree_contribution.value >= 0);
        assert(subtree_contribution.value <= context.tree_contribution.value);
        assert(subtree_contribution.sub <= context.tree_contribution.sub);

        WeightedScores score;
        score.value = ConvexF<ContributionAmount>(
                subtree_contribution.value / context.tree_contribution.value,
                context.B,
                context.S);

        score.sub = ConvexF<ContributionAmount>(
                subtree_contribution.sub / context.tree_contribution.sub,
                context.B,
                context.S);

        score.tree_size = subtree_contribution.tree_size;
        
        assert(score.value >= 0);
        assert(score.sub >= 0);
        assert(score.tree_size > 0);
        return score;
    }

    struct ExpectedValues
    {
        ContributionAmount value;
        ContributionAmount sub;
        size_t tree_size;
    };

    ExpectedValues ExpectedValue(
            CGSContext& context,
            const char address_type,
            const referral::Address& address,
            referral::ReferralsViewCache& db)
    {
        //this case can occur on regtest if there is not enough data.
        if (context.tree_contribution.value == 0) {
            return {0, 0, 0};
        }

        assert(context.tree_contribution.value > 0);
        assert(context.tree_contribution.sub > 0);

        auto expected_value = WeightedScore(
                context,
                address_type,
                address,
                db);

        assert(expected_value.value >= 0);
        assert(expected_value.sub >= 0);

        const auto children = db.GetChildren(address);

        for (const auto& c:  children) {
            auto maybe_ref = db.GetReferral(c);
            if (!maybe_ref) {
                continue;
            }

            auto child_score = WeightedScore(
                    context,
                    maybe_ref->addressType,
                    maybe_ref->GetAddress(),
                    db);

            assert(child_score.value >= 0);
            assert(child_score.sub >= 0);
            expected_value.value -= child_score.value;
            expected_value.sub -= child_score.sub;
        }

        assert(expected_value.value >= 0);
        assert(expected_value.sub >= 0);
        return { 
            expected_value.value,
                expected_value.sub,
                expected_value.tree_size
        };
    }

    Entrant ComputeCGS(
            CGSContext& context,
            char address_type,
            const referral::Address& address,
            referral::ReferralsViewCache& db)
    {
        auto expected_value = ExpectedValue(
                context,
                address_type,
                address,
                db);

        const ContributionAmount cgs = context.tree_contribution.value * expected_value.value;
        const ContributionAmount sub_cgs = context.tree_contribution.sub * expected_value.sub;

        assert(cgs >= 0);
        assert(sub_cgs >= 0);

        auto floored_cgs = cgs.convert_to<CAmount>();
        auto floored_sub_cgs = sub_cgs.convert_to<CAmount>();

        const auto balance = GetAgedBalance(
                context,
                address_type,
                address);

        const auto children = db.GetChildren(address);
        const auto height = GetReferralHeight(db, address);

        return Entrant{
            address_type,
                address,
                balance.second,
                balance.first,
                floored_cgs,
                floored_sub_cgs,
                height,
                children.size(),
                expected_value.tree_size
        };
    }

    void GetAllRewardableEntrants(
            CGSContext& context,
            referral::ReferralsViewCache& db,
            const Consensus::Params& params,
            int height,
            Entrants& entrants)
    {
        assert(height >= 0);

        referral::AddressANVs anv_entrants;

        db.GetAllRewardableANVs(params, NO_GENESIS, anv_entrants);

        entrants.resize(anv_entrants.size());

        context.tip_height = height;
        GetAllCoins(height, context.coins);

        context.coin_maturity = params.pog2_coin_maturity;
        context.new_coin_maturity = params.pog2_new_coin_maturity;
        context.tree_contribution = ContributionSubtreeIter(context, 2, params.genesis_address, db);
        context.B = params.pog2_convex_b;
        context.S = params.pog2_convex_s;


        std::transform(anv_entrants.begin(), anv_entrants.end(), entrants.begin(),
                [height, &db, &context, &params](const referral::AddressANV& a) {
                    auto entrant = ComputeCGS(
                            context,
                            a.address_type,
                            a.address,
                            db);

                    auto height = GetReferralHeight(db, a.address);
                    assert(height >= 0);
                    entrant.beacon_height = height;
                    return entrant;
                });
    }

} // namespace pog2
