// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The bitcoin-chainstate executable serves to surface the dependencies required
// by a program wishing to use Bitcoin Core's consensus engine as it is right
// now.
//
// DEVELOPER NOTE: Since this is a "demo-only", experimental, etc. executable,
//                 it may diverge from Bitcoin Core's coding style.
//
// It is part of the libbitcoinkernel project.

#include <chainparams.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <init/common.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <scheduler.h>
#include <script/sigcache.h>
#include <util/system.h>
#include <util/thread.h>
#include <validation.h>
#include <validationinterface.h>

#include <filesystem>
#include <functional>
#include <iosfwd>

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;

int main(int argc, char* argv[])
{
    // SETUP: Argument parsing and handling
    if (argc != 2) {
        std::cerr
            << "Usage: " << argv[0] << " DATADIR" << std::endl
            << "Display DATADIR information, and process hex-encoded blocks on standard input." << std::endl
            << std::endl
            << "IMPORTANT: THIS EXECUTABLE IS EXPERIMENTAL, FOR TESTING ONLY, AND EXPECTED TO" << std::endl
            << "           BREAK IN FUTURE VERSIONS. DO NOT USE ON YOUR ACTUAL DATADIR." << std::endl;
        return 1;
    }
    std::filesystem::path abs_datadir = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(abs_datadir);
    gArgs.ForceSetArg("-datadir", abs_datadir.string());


    // SETUP: Misc Globals
    SelectParams(CBaseChainParams::MAIN);
    const CChainParams& chainparams = Params();

    init::SetGlobals(); // ECC_Start, etc.

    // Necessary for CheckInputScripts (eventually called by ProcessNewBlock),
    // which will try the script cache first and fall back to actually
    // performing the check with the signature cache.
    InitSignatureCache();
    InitScriptExecutionCache();


    // SETUP: Scheduling and Background Signals
    CScheduler scheduler{};
    // Start the lightweight task scheduler thread
    scheduler.m_service_thread = std::thread(util::TraceThread, "scheduler", [&] { scheduler.serviceQueue(); });

    // Gather some entropy once per minute.
    scheduler.scheduleEvery(RandAddPeriodic, std::chrono::minutes{1});

    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);


    // SETUP: Chainstate
    ChainstateManager chainman;

    auto rv = node::LoadChainstate(false,
                                   std::ref(chainman),
                                   nullptr,
                                   false,
                                   true,
                                   chainparams.GetConsensus(),
                                   false,
                                   2 << 20,
                                   2 << 22,
                                   (450 << 20) - (2 << 20) - (2 << 22),
                                   false,
                                   false,
                                   []() { return false; });
    if (rv.has_value()) {
        std::cerr << "Failed to load Chain state from your datadir." << std::endl;
        goto epilogue;
    } else {
        auto maybe_verify_error = node::VerifyLoadedChainstate(std::ref(chainman),
                                                               false,
                                                               false,
                                                               chainparams.GetConsensus(),
                                                               DEFAULT_CHECKBLOCKS,
                                                               DEFAULT_CHECKLEVEL,
                                                               /*get_unix_time_seconds=*/static_cast<int64_t (*)()>(GetTime));
        if (maybe_verify_error.has_value()) {
            std::cerr << "Failed to verify loaded Chain state from your datadir." << std::endl;
            goto epilogue;
        }
    }

    for (CChainState* chainstate : WITH_LOCK(::cs_main, return chainman.GetAll())) {
        BlockValidationState state;
        if (!chainstate->ActivateBestChain(state, nullptr)) {
            std::cerr << "Failed to connect best block (" << state.ToString() << ")" << std::endl;
            goto epilogue;
        }
    }

    // Main program logic starts here
    std::cout
        << "Hello! I'm going to print out some information about your datadir." << std::endl
        << "\t" << "Path: " << gArgs.GetDataDirNet() << std::endl
        << "\t" << "Reindexing: " << std::boolalpha << node::fReindex.load() << std::noboolalpha << std::endl
        << "\t" << "Snapshot Active: " << std::boolalpha << chainman.IsSnapshotActive() << std::noboolalpha << std::endl
        << "\t" << "Active Height: " << chainman.ActiveHeight() << std::endl
        << "\t" << "Active IBD: " << std::boolalpha << chainman.ActiveChainstate().IsInitialBlockDownload() << std::noboolalpha << std::endl;
    {
        CBlockIndex* tip = chainman.ActiveTip();
        if (tip) {
            std::cout << "\t" << tip->ToString() << std::endl;
        }
    }

    for (std::string line; std::getline(std::cin, line);) {
        if (line.empty()) {
            std::cerr << "Empty line found" << std::endl;
            break;
        }

        std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
        CBlock& block = *blockptr;

        if (!DecodeHexBlk(block, line)) {
            std::cerr << "Block decode failed" << std::endl;
            break;
        }

        if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
            std::cerr << "Block does not start with a coinbase" << std::endl;
            break;
        }

        uint256 hash = block.GetHash();
        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);
            if (pindex) {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
                    std::cerr << "duplicate" << std::endl;
                    break;
                }
                if (pindex->nStatus & BLOCK_FAILED_MASK) {
                    std::cerr << "duplicate-invalid" << std::endl;
                    break;
                }
            }
        }

        {
            LOCK(cs_main);
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
            if (pindex) {
                UpdateUncommittedBlockStructures(block, pindex, chainparams.GetConsensus());
            }
        }

        // Adapted from rpc/mining.cpp
        class submitblock_StateCatcher final : public CValidationInterface
        {
        public:
            uint256 hash;
            bool found;
            BlockValidationState state;

            explicit submitblock_StateCatcher(const uint256& hashIn) : hash(hashIn), found(false), state() {}

        protected:
            void BlockChecked(const CBlock& block, const BlockValidationState& stateIn) override
            {
                if (block.GetHash() != hash)
                    return;
                found = true;
                state = stateIn;
            }
        };

        bool new_block;
        auto sc = std::make_shared<submitblock_StateCatcher>(block.GetHash());
        RegisterSharedValidationInterface(sc);
        bool accepted = chainman.ProcessNewBlock(chainparams, blockptr, /* force_processing */ true, /* new_block */ &new_block);
        UnregisterSharedValidationInterface(sc);
        if (!new_block && accepted) {
            std::cerr << "duplicate" << std::endl;
            break;
        }
        if (!sc->found) {
            std::cerr << "inconclusive" << std::endl;
            break;
        }
        std::cout << sc->state.ToString() << std::endl;
        switch (sc->state.GetResult()) {
        case BlockValidationResult::BLOCK_RESULT_UNSET:
            std::cerr << "initial value. Block has not yet been rejected" << std::endl;
            break;
        case BlockValidationResult::BLOCK_CONSENSUS:
            std::cerr << "invalid by consensus rules (excluding any below reasons)" << std::endl;
            break;
        case BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE:
            std::cerr << "Invalid by a change to consensus rules more recent than SegWit." << std::endl;
            break;
        case BlockValidationResult::BLOCK_CACHED_INVALID:
            std::cerr << "this block was cached as being invalid and we didn't store the reason why" << std::endl;
            break;
        case BlockValidationResult::BLOCK_INVALID_HEADER:
            std::cerr << "invalid proof of work or time too old" << std::endl;
            break;
        case BlockValidationResult::BLOCK_MUTATED:
            std::cerr << "the block's data didn't match the data committed to by the PoW" << std::endl;
            break;
        case BlockValidationResult::BLOCK_MISSING_PREV:
            std::cerr << "We don't have the previous block the checked one is built on" << std::endl;
            break;
        case BlockValidationResult::BLOCK_INVALID_PREV:
            std::cerr << "A block this one builds on is invalid" << std::endl;
            break;
        case BlockValidationResult::BLOCK_TIME_FUTURE:
            std::cerr << "block timestamp was > 2 hours in the future (or our clock is bad)" << std::endl;
            break;
        case BlockValidationResult::BLOCK_CHECKPOINT:
            std::cerr << "the block failed to meet one of our checkpoints" << std::endl;
            break;
        }
    }

epilogue:
    // Without this precise shutdown sequence, there will be a lot of nullptr
    // dereferencing and UB.
    scheduler.stop();
    if (chainman.m_load_block.joinable()) chainman.m_load_block.join();
    StopScriptCheckWorkerThreads();

    GetMainSignals().FlushBackgroundCallbacks();
    {
        LOCK(cs_main);
        for (CChainState* chainstate : chainman.GetAll()) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }
    GetMainSignals().UnregisterBackgroundSignalScheduler();

    WITH_LOCK(::cs_main, UnloadBlockIndex(nullptr, chainman));

    init::UnsetGlobals();
}
