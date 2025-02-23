// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionrecord.h>

#include <chain.h>
#include <chainparams.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <names/encoding.h>
#include <script/names.h>
#include <wallet/ismine.h>

#include <stdint.h>

#include <QDateTime>

using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    if (nNet > 0 || wtx.is_coinbase)
    {
        //
        // Credit
        //
        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CTxOut& txout = wtx.tx->vout[i];
            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                TransactionRecord sub(hash, nTime);
                sub.idx = i; // vout index
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (wtx.txout_address_is_mine[i])
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                if (wtx.is_coinbase)
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                }

                parts.append(sub);
            }
        }
    }
    else
    {
        bool involvesWatchAddress = false;
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllToMe > mine) fAllToMe = mine;
        }

        std::optional<CNameScript> nNameCredit = wtx.name_credit;
        std::optional<CNameScript> nNameDebit = wtx.name_debit;

        TransactionRecord nameSub(hash, nTime, TransactionRecord::NameOp, "", 0, 0);

        if(nNameCredit)
        {
            // TODO: Use friendly names based on namespaces
            if(nNameCredit.value().isAnyUpdate())
            {
                if(nNameDebit)
                {
                    if(nNameCredit.value().getNameOp() == OP_NAME_FIRSTUPDATE)
                    {
                        nameSub.nameOpType = TransactionRecord::NameOpType::FirstUpdate;
                    }
                    else
                    {
                        // OP_NAME_UPDATE

                        // Check if renewal (previous value is unchanged)
                        if(nNameDebit.value().isAnyUpdate() && nNameDebit.value().getOpValue() == nNameCredit.value().getOpValue())
                        {
                            nameSub.nameOpType = TransactionRecord::NameOpType::Renew;
                        }
                        else
                        {
                            nameSub.nameOpType = TransactionRecord::NameOpType::Update;
                        }
                    }
                }
                else
                {
                    nameSub.nameOpType = TransactionRecord::NameOpType::Recv;
                }

                nameSub.address = EncodeNameForMessage(nNameCredit.value().getOpName());
            }
            else
            {
                nameSub.nameOpType = TransactionRecord::NameOpType::New;
            }
        }
        else if(nNameDebit)
        {
            nameSub.nameOpType = TransactionRecord::NameOpType::Send;

            if(nNameDebit.value().isAnyUpdate())
            {
                nameSub.address = EncodeNameForMessage(nNameDebit.value().getOpName());
            }
        }

        if (fAllFromMe && fAllToMe)
        {
            // Payment to self
            std::string address;
            for (auto it = wtx.txout_address.begin(); it != wtx.txout_address.end(); ++it) {
                if (it != wtx.txout_address.begin()) address += ", ";
                address += EncodeDestination(*it);
            }

            CAmount nChange = wtx.change;

            if(nNameCredit)
            {
                nameSub.debit = -(nDebit - nChange);
                nameSub.credit = nCredit - nChange;
                parts.append(nameSub);
            }
            else
            {
                parts.append(TransactionRecord(hash, nTime, TransactionRecord::SendToSelf, address, -(nDebit - nChange), nCredit - nChange));
            }

            parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
        }
        else if (fAllFromMe)
        {
            //
            // Debit
            //
            CAmount nTxFee = nDebit - wtx.tx->GetValueOut();

            for (unsigned int nOut = 0; nOut < wtx.tx->vout.size(); nOut++)
            {
                const CTxOut& txout = wtx.tx->vout[nOut];
                TransactionRecord sub(hash, nTime);
                sub.idx = nOut;
                sub.involvesWatchAddress = involvesWatchAddress;

                if(wtx.txout_is_mine[nOut])
                {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    continue;
                }

                if(nNameDebit && CNameScript::isNameScript(txout.scriptPubKey))
                {
                    nameSub.idx = sub.idx;
                    nameSub.involvesWatchAddress = sub.involvesWatchAddress;
                    sub = nameSub;
                }
                else if (!std::get_if<CNoDestination>(&wtx.txout_address[nOut]))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[nOut]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }

                CAmount nValue = txout.nValue;
                if (sub.type == TransactionRecord::NameOp)
                {
                    // 300k is just a "sufficiently high" height
                    nValue -= Params().GetConsensus().rules->MinNameCoinAmount(300000);
                }
                /* Add fee to first output */
                if (nTxFee > 0)
                {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                parts.append(sub);
            }
        }
        else
        {
            // Mixed debit transaction, can't break down payees

            if(nNameCredit)
            {
                nameSub.debit = nNet;
                parts.append(nameSub);
            }
            else
            {
                parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
            }

            parts.last().involvesWatchAddress = involvesWatchAddress;
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.m_cur_block_hash = block_hash;

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated) {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(const uint256& block_hash) const
{
    assert(!block_hash.IsNull());
    return status.m_cur_block_hash != block_hash || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
