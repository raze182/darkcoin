

#include "darksend.h"
#include "main.h"
#include "init.h"
#include "masternode.h"
#include "activemasternode.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;


/** The main object for accessing darksend */
CDarkSendPool darkSendPool;
/** A helper object for signing messages from masternodes */
CDarkSendSigner darkSendSigner;
/** All denominations used by darksend */
std::vector<int64> darkSendDenominations;
/** The current darksends in progress on the network */
std::vector<CDarksendQueue> vecDarksendQueue;
/** Keep track of the used masternodes */
std::vector<CTxIn> vecMasternodesUsed;
// count peers we've requested the list from
int RequestedMasterNodeList = 0; 

/* *** BEGIN DARKSEND MAGIC - DARKCOIN **********
    Copyright 2014, Darkcoin Developers 
        eduffield - evan@darkcoin.io
*/

void ProcessMessageDarksend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == "dsf") { //DarkSend Final tx  
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(darkSendPool.submittedToMasternode != pfrom->addr){
            //LogPrintf("dsc - message doesn't match current masternode - %s != %s\n", darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        CTransaction txNew;
        vRecv >> sessionID >> txNew;

        if(darkSendPool.sessionID != sessionID){
            if (fDebug) LogPrintf("dsf - message doesn't match current darksend session %d %d\n", darkSendPool.sessionID, sessionID);
            return;
        }

        //check to see if input is spent already? (and probably not confirmed)
        darkSendPool.SignFinalTransaction(txNew, pfrom);
    }

    else if (strCommand == "dsc") { //DarkSend Complete
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(darkSendPool.submittedToMasternode != pfrom->addr){
            //LogPrintf("dsc - message doesn't match current masternode - %s != %s\n", darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        bool error;
        std::string lastMessage;
        vRecv >> sessionID >> error >> lastMessage;

        if(darkSendPool.sessionID != sessionID){
            if (fDebug) LogPrintf("dsc - message doesn't match current darksend session %d %d\n", darkSendPool.sessionID, sessionID);
            return;
        }

        darkSendPool.CompletedTransaction(error, lastMessage);
    }

    else if (strCommand == "dsa") { //DarkSend Acceptable
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(!fMasterNode){
            return;
        }

        int nDenom;
        CTransaction txCollateral;
        vRecv >> nDenom >> txCollateral;

        std::string error = "";
        int mn = GetMasternodeByVin(activeMasternode.vinMasternode);
        if(mn == -1){
            std::string strError = "Not in the masternode list";
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;            
        }

        if(darkSendMasterNodes[mn].nLastDsq != 0 && 
            darkSendMasterNodes[mn].nLastDsq + (int)darkSendMasterNodes.size()/5 > darkSendPool.nDsqCount){
            //LogPrintf("dsa -- last dsq too recent, must wait. %s \n", darkSendMasterNodes[mn].addr.ToString().c_str());
            std::string strError = "Last darksend was too recent";
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;
        }

        if(!darkSendPool.IsCompatibleWithSession(nDenom, txCollateral, error))
        {
            LogPrintf("dsa -- not compatible with existing transactions! \n");
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        } else {
            LogPrintf("dsa -- is compatible, please submit! \n");
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_ACCEPTED, error);
            return;
        }
    } else if (strCommand == "dsq") { //DarkSend Queue
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        CDarksendQueue dsq;
        vRecv >> dsq;

        CService addr;
        if(!dsq.GetAddress(addr)) return;
        if(!dsq.CheckSignature()) return;

        if(dsq.IsExpired()) return;

        int mn = GetMasternodeByVin(dsq.vin);
        if(mn == -1) return;


        // if the queue is ready, submit if we can
        if(dsq.ready) {
            if(darkSendPool.submittedToMasternode != addr){
                LogPrintf("dsq - message doesn't match current masternode - %s != %s\n", darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
                return;
            }

            if (fDebug)  LogPrintf("darksend queue is ready - %s\n", addr.ToString().c_str());

            darkSendPool.DoAutomaticDenominating(false, true);
        } else {
            BOOST_FOREACH(CDarksendQueue q, vecDarksendQueue){
                if(q.vin == dsq.vin) return;
            }
            
            //don't allow a few nodes to dominate the queuing process
            darkSendPool.nDsqCount++;
            if(darkSendMasterNodes[mn].nLastDsq != 0 && 
                darkSendMasterNodes[mn].nLastDsq + (int)darkSendMasterNodes.size()/5 > darkSendPool.nDsqCount){
                LogPrintf("dsq -- masternode sending too many dsq messages. %s \n", darkSendMasterNodes[mn].addr.ToString().c_str());
                return;
            }
            darkSendMasterNodes[mn].nLastDsq = darkSendPool.nDsqCount; 
            darkSendMasterNodes[mn].allowFreeTx = true;

            if (fDebug)  LogPrintf("new darksend queue object - %s\n", addr.ToString().c_str());
            vecDarksendQueue.push_back(dsq);
            dsq.Relay();
            dsq.time = GetTime();
        }

    } else if (strCommand == "dsi") { //DarkSend vIn
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(!fMasterNode){
            return;
        }

        std::vector<CTxIn> in;
        int64 nAmount;
        CTransaction txCollateral;
        std::vector<CTxOut> out;
        vRecv >> in >> nAmount >> txCollateral >> out;

        std::string error = "";

        //do we have enough users in the current session?
        if(!darkSendPool.IsSessionReady()){
            LogPrintf("dsi -- session not complete! \n");
            error = "session not complete!";
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        //do we have the same denominations as the current session?
        if(!darkSendPool.IsCompatibleWithEntries(out))
        {
            LogPrintf("dsi -- not compatible with existing transactions! \n");
            error = "not compatible with existing transactions";
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }
        
        //check it like a transaction
        {
            int64 nValueIn = 0;
            int64 nValueOut = 0;
            bool missingTx = false;

            CValidationState state;
            CTransaction tx;

            BOOST_FOREACH(const CTxOut o, out){
                nValueOut += o.nValue;
                tx.vout.push_back(o);
                
                if(o.scriptPubKey.size() != 25){
                    LogPrintf("dsi - non-standard pubkey detected! %s\n", o.scriptPubKey.ToString().c_str());
                    error = "non-standard pubkey detected";
                    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }

            }

            BOOST_FOREACH(const CTxIn i, in){
                tx.vin.push_back(i);

                LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());                

                CTransaction tx2;
                uint256 hash;
                if(GetTransaction(i.prevout.hash, tx2, hash, true)){
                    if(tx2.vout.size() > i.prevout.n) {
                        nValueIn += tx2.vout[i.prevout.n].nValue;    
                    }
                } else{
                    missingTx = true;
                }
            }

            if(!missingTx){
                if (nValueIn-nValueOut > nValueIn*.01) {
                    LogPrintf("dsi -- fees are too high! %s\n", tx.ToString().c_str());
                    error = "transaction fees are too high";
                    pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
            } else {
                LogPrintf("dsi -- missing input tx! %s\n", tx.ToString().c_str());
                error = "missing input tx information";
                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }

            bool missing = false;
            if (!tx.IsAcceptable(state, true, false, &missing, false)){ //AcceptableInputs(state, true)){
                LogPrintf("dsi -- transactione not valid! \n");
                error = "transaction not valid";
                pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }
        }

        if(darkSendPool.AddEntry(in, nAmount, txCollateral, out, error)){
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_ACCEPTED, error);
            darkSendPool.Check();

            RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        } else {
            pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_REJECTED, error);
        }
    }

    else if (strCommand == "dssub") { //DarkSend Subscribe To         
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(!fMasterNode) return;

        std::string error = "";
        pfrom->PushMessage("dssu", darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET, error);
        //pfrom->fDisconnect = true;
        return;
    }

    else if (strCommand == "dssu") { //DarkSend status update
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        if(darkSendPool.submittedToMasternode != pfrom->addr){   
            //LogPrintf("dssu - message doesn't match current masternode - %s != %s\n", darkSendPool.submittedToMasternode.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionID;
        int state;
        int entriesCount;
        int accepted;
        std::string error;
        vRecv >> sessionID >> state >> entriesCount >> accepted >> error;

        LogPrintf("dssu - state: %i entriesCount: %i accepted: %i error: %s \n", state, entriesCount, accepted, error.c_str());

        if((accepted != 1 && accepted != 0) && darkSendPool.sessionID != sessionID){
            LogPrintf("dssu - message doesn't match current darksend session %d %d\n", darkSendPool.sessionID, sessionID);
            return;
        }
        
        darkSendPool.StatusUpdate(state, entriesCount, accepted, error, sessionID);

    }

    else if (strCommand == "dss") { //DarkSend Sign Final Tx
        if (pfrom->nVersion != darkSendPool.MIN_PEER_PROTO_VERSION) {
            return;
        }

        vector<CTxIn> sigs;
        vRecv >> sigs;

        bool success = false;
        int count = 0;

        LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);

        BOOST_FOREACH(const CTxIn item, sigs)
        {
            if(darkSendPool.AddScriptSig(item)) success = true;
            LogPrintf(" -- sigs count %d %d\n", (int)sigs.size(), count);
            count++;
        }

        if(success){
            darkSendPool.Check();
            RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        }
    }

}

int randomizeList (int i) { return std::rand()%i;}

// Recursively determine the rounds of a given input (How deep is the darksend chain for a given input)
int GetInputDarksendRounds(CTxIn in, int rounds)
{
    if(rounds >= 9) return rounds;

    std::string padding = "";
    padding.insert(0, ((rounds+1)*5)+3, ' ');

    CWalletTx tx;
    if(pwalletMain->GetTransaction(in.prevout.hash,tx)){
        // bounds check
        if(in.prevout.n >= tx.vout.size()) return -4;

        if(tx.vout[in.prevout.n].nValue == DARKSEND_FEE) return -3;

        if(rounds == 0){ //make sure the final output is non-denominate
            bool found = false;
            BOOST_FOREACH(int64 d, darkSendDenominations)
                if(tx.vout[in.prevout.n].nValue == d) found = true;

            if(!found) {
                //LogPrintf(" - NOT DENOM\n");
                return -2;
            }
        }
        bool found = false;

        BOOST_FOREACH(CTxOut out, tx.vout){
            BOOST_FOREACH(int64 d, darkSendDenominations)
                if(out.nValue == d)
                    found = true;
        }
        
        if(!found) {
            //LogPrintf(" - NOT FOUND\n");
            return rounds;
        }

        // find my vin and look that up
        BOOST_FOREACH(CTxIn in2, tx.vin) {
            if(pwalletMain->IsMine(in2)){
                //LogPrintf("rounds :: %s %s %d NEXT\n", padding.c_str(), in.ToString().c_str(), rounds);  
                int n = GetInputDarksendRounds(in2, rounds+1);
                if(n != -3) return n;
            }
        }
    } else {
        //LogPrintf("rounds :: %s %s %d NOTFOUND\n", padding.c_str(), in.ToString().c_str(), rounds);
    }

    return rounds-1;
}

void CDarkSendPool::SetNull(bool clearEverything){
    finalTransaction.vin.clear();
    finalTransaction.vout.clear();

    entries.clear();

    state = POOL_STATUS_ACCEPTING_ENTRIES;

    lastTimeChanged = GetTimeMillis();

    entriesCount = 0;
    lastEntryAccepted = 0;
    countEntriesAccepted = 0;

    sessionUsers = 0;
    sessionDenom = 0;
    sessionFoundMasternode = false;
    sessionTries = 0;
    vecSessionCollateral.clear();
    txCollateral = CTransaction();

    if(clearEverything){
        myEntries.clear();

        if(fMasterNode){
            sessionID = 1 + (rand() % 999999);
        } else {
            sessionID = 0;
        }
    }

    // -- seed random number generator (used for ordering output lists)
    unsigned int seed = 0;
    RAND_bytes((unsigned char*)&seed, sizeof(seed));
    std::srand(seed);
}

bool CDarkSendPool::SetCollateralAddress(std::string strAddress){
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
    {
        LogPrintf("CDarkSendPool::SetCollateralAddress - Invalid DarkSend collateral address\n");
        return false;
    }
    collateralPubKey.SetDestination(address.Get());
    return true;
}

//
// Unlock coins after Darksend fails or succeeds
//
void CDarkSendPool::UnlockCoins(){
    BOOST_FOREACH(CTxIn v, lockedCoins)
        pwalletMain->UnlockCoin(v.prevout);

    lockedCoins.clear();
}

//
// Check the Darksend progress and send client updates if a masternode
// 
void CDarkSendPool::Check()
{
    if(fDebug) LogPrintf("CDarkSendPool::Check()\n");
    if(fDebug) LogPrintf("CDarkSendPool::Check() - entries count %lu\n", entries.size());
    
    // If entries is full, then move on to the next phase
    if(state == POOL_STATUS_ACCEPTING_ENTRIES && (int)entries.size() >= GetMaxPoolTransactions())
    {
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- ACCEPTING OUTPUTS\n");
        UpdateState(POOL_STATUS_FINALIZE_TRANSACTION);
    }

    // create the finalized transaction for distribution to the clients
    if(state == POOL_STATUS_FINALIZE_TRANSACTION && finalTransaction.vin.empty() && finalTransaction.vout.empty()) {
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- FINALIZE TRANSACTIONS\n");
        UpdateState(POOL_STATUS_SIGNING);

        if (fMasterNode) { 
            // make our new transaction
            CTransaction txNew;
            for(unsigned int i = 0; i < entries.size(); i++){
                BOOST_FOREACH(const CTxOut v, entries[i].vout)
                    txNew.vout.push_back(v);

                BOOST_FOREACH(const CDarkSendEntryVin s, entries[i].sev)
                    txNew.vin.push_back(s.vin);
            }
            // shuffle the outputs for improved anonymity
            std::random_shuffle ( txNew.vout.begin(), txNew.vout.end(), randomizeList);

            if(fDebug) LogPrintf("Transaction 1: %s\n", txNew.ToString().c_str());

            SignFinalTransaction(txNew, NULL);

            // request signatures from clients
            RelayDarkSendFinalTransaction(sessionID, txNew);
        }
    }

    // collect signatures from clients

    // If we have all of the signatures, try to compile the transaction
    if(state == POOL_STATUS_SIGNING && SignaturesComplete()) { 
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- SIGNING\n");            
        UpdateState(POOL_STATUS_TRANSMISSION);

        CWalletTx txNew = CWalletTx(pwalletMain, finalTransaction);

        LOCK2(cs_main, pwalletMain->cs_wallet);
        {
            if (fMasterNode) { //only the main node is master atm                
                if(fDebug) LogPrintf("Transaction 2: %s\n", txNew.ToString().c_str());

                // See if the transaction is valid
                if (!txNew.AcceptToMemoryPool(true, false))
                {
                    LogPrintf("CDarkSendPool::Check() - CommitTransaction : Error: Transaction not valid\n");
                    SetNull();
                    pwalletMain->Lock();

                    // not much we can do in this case
                    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
                    RelayDarkSendCompletedTransaction(sessionID, true, "Transaction not valid, please try again");
                    return;
                }

                LogPrintf("CDarkSendPool::Check() -- IS MASTER -- TRANSMITTING DARKSEND\n");

                // sign a message

                int64 sigTime = GetAdjustedTime();
                std::string strMessage = txNew.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
                std::string strError = "";
                std::vector<unsigned char> vchSig;
                CKey key2;
                CPubKey pubkey2;

                if(!darkSendSigner.SetKey(strMasterNodePrivKey, strError, key2, pubkey2))
                {
                    LogPrintf("Invalid masternodeprivkey: '%s'\n", strError.c_str());
                    exit(0);
                }

                if(!darkSendSigner.SignMessage(strMessage, strError, vchSig, key2)) {
                    LogPrintf("CDarkSendPool::Check() - Sign message failed\n");
                    return;
                }

                if(!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, strError)) {
                    LogPrintf("CDarkSendPool::Check() - Verify message failed\n");
                    return;
                }

                // Broadcast the transaction to the network
                txNew.AddSupportingTransactions();
                txNew.fTimeReceivedIsTxTime = true;                
                RelayDarkSendTransaction((CTransaction)txNew, activeMasternode.vinMasternode, vchSig, sigTime);

                // Tell the clients it was successful                
                RelayDarkSendCompletedTransaction(sessionID, false, "Transaction Created Successfully");

                // Randomly charge clients
                ChargeRandomFees();
            }
        }
    }

    // move on to next phase, allow 3 seconds incase the masternode wants to send us anything else
    if((state == POOL_STATUS_TRANSMISSION && fMasterNode) || (state == POOL_STATUS_SIGNING && completedTransaction) ) {
        LogPrintf("CDarkSendPool::Check() -- COMPLETED -- RESETTING \n");
        SetNull(true);
        UnlockCoins();
        if(fMasterNode) RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        pwalletMain->Lock();
    }

    // reset if we're here for 10 seconds
    if((state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) && GetTimeMillis()-lastTimeChanged >= 10000) {
        LogPrintf("CDarkSendPool::Check() -- RESETTING MESSAGE \n");
        SetNull(true);
        if(fMasterNode) RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        UnlockCoins();
    }
}

//
// Charge clients a fee if they're abusive
//
// Why bother? Darksend uses collateral to ensure abuse to the process is kept to a minimum. 
// The submission and signing stages in darksend are completely separate. In the cases where 
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
// 
// How does this work? Messages to masternodes come in via "dsi", these require a valid collateral 
// transaction for the client to be able to enter the pool. This transaction is kept by the masternode
// until the transaction is either complete or fails. 
//
void CDarkSendPool::ChargeFees(){    
    if(fMasterNode) {
        int i = 0;

        if(state == POOL_STATUS_ACCEPTING_ENTRIES){
            BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
                bool found = false;
                BOOST_FOREACH(const CDarkSendEntry& v, entries) {
                    if(v.collateral == txCollateral) {
                        found = true;
                    }
                }

                // This queue entry didn't send us the promised transaction
                if(!found){
                    LogPrintf("CDarkSendPool::ChargeFees -- found uncooperative node (didn't send transaction). charging fees. %u\n", i);

                    CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                    // Broadcast
                    if (!wtxCollateral.AcceptToMemoryPool(true, false))
                    {
                        // This must not fail. The transaction has already been signed and recorded.
                        LogPrintf("CDarkSendPool::ChargeFees() : Error: Transaction not valid");
                    }
                    wtxCollateral.RelayWalletTransaction();
                    i++;
                }
            }
        }

        if(state == POOL_STATUS_SIGNING) {
            // who didn't sign?
            BOOST_FOREACH(const CDarkSendEntry v, entries) {
                BOOST_FOREACH(const CDarkSendEntryVin s, v.sev) {
                    if(!s.isSigSet){
                        LogPrintf("CDarkSendPool::ChargeFees -- found uncooperative node (didn't sign). charging fees. %u\n", i);

                        CWalletTx wtxCollateral = CWalletTx(pwalletMain, v.collateral);

                        // Broadcast
                        if (!wtxCollateral.AcceptToMemoryPool(true, false))
                        {
                            // This must not fail. The transaction has already been signed and recorded.
                            LogPrintf("CDarkSendPool::ChargeFees() : Error: Transaction not valid");
                        }
                        wtxCollateral.RelayWalletTransaction();
                    }
                    i++;
                }
            }
        }
    }
}

// charge the collateral randomly
//  - Darksend is completely free, to pay miners we randomly pay the collateral of users.
void CDarkSendPool::ChargeRandomFees(){    
    if(fMasterNode) {
        int i = 0;

        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
            int r = rand()%1000;

            if(r <= 20)
            {
                LogPrintf("CDarkSendPool::ChargeRandomFees -- charging random fees. %u\n", i);

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true, false))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CDarkSendPool::ChargeRandomFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
            }
        }
    }
}

//
// Check for various timeouts (queue objects, darksend, etc)
//
void CDarkSendPool::CheckTimeout(){
    // catching hanging sessions
    if(!fMasterNode) {
        if(state == POOL_STATUS_TRANSMISSION) {
            if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session complete -- Running Check()\n");
            Check();
        }        
    }

    // check darksend queue objects for timeouts
    int c = 0;
    vector<CDarksendQueue>::iterator it;
    for(it=vecDarksendQueue.begin();it<vecDarksendQueue.end();it++){
        if((*it).IsExpired()){
            if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() : Removing expired queue entry - %d\n", c);
            vecDarksendQueue.erase(it);
            break;
        }
        c++;
    }

    /* Check to see if we're ready for submissions from clients */
    if(state == POOL_STATUS_QUEUE && sessionUsers == GetMaxPoolTransactions()) {
        CDarksendQueue dsq;
        dsq.nDenom = sessionDenom;
        dsq.vin = activeMasternode.vinMasternode;
        dsq.time = GetTime();
        dsq.ready = true;
        dsq.Sign();
        dsq.Relay();

        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
    }

    int addLagTime = 0;
    if(!fMasterNode) addLagTime = 10000; //if we're the client, give the server a few extra seconds before resetting.

    if(state == POOL_STATUS_ACCEPTING_ENTRIES || state == POOL_STATUS_QUEUE){
        c = 0;

        // if it's a masternode, the entries are stored in "entries", otherwise they're stored in myEntries
        std::vector<CDarkSendEntry> *vec = &myEntries;
        if(fMasterNode) vec = &entries; 

        // check for a timeout and reset if needed
        vector<CDarkSendEntry>::iterator it2;
        for(it2=vec->begin();it2<vec->end();it2++){
            if((*it2).IsExpired()){
                if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() : Removing expired entry - %d\n", c);
                vec->erase(it2);
                if(entries.size() == 0 && myEntries.size() == 0){
                    SetNull(true);
                    UnlockCoins();
                }
                if(fMasterNode){
                    RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);   
                }
                break;
            }
            c++;
        }

        if(GetTimeMillis()-lastTimeChanged >= 30000+addLagTime){
            lastTimeChanged = GetTimeMillis();

            ChargeFees();  
            // reset session information for the queue query stage (before entering a masternode, clients will send a queue request to make sure they're compatible denomination wise)
            sessionUsers = 0;
            sessionDenom = 0;
            sessionFoundMasternode = false;
            sessionTries = 0;            
            vecSessionCollateral.clear();

            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
        }
    } else if(GetTimeMillis()-lastTimeChanged >= 30000+addLagTime){
        if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session timed out (30s) -- resetting\n");
        SetNull();
        UnlockCoins();

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = "Session timed out (30), please resubmit";
    }


    if(state == POOL_STATUS_SIGNING && GetTimeMillis()-lastTimeChanged >= 10000+addLagTime ) {
        if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session timed out -- restting\n");
        ChargeFees();
        SetNull();
        UnlockCoins();
        //add my transactions to the new session

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = "Signing timed out, please resubmit";
    }
}

// check to see if the signature is valid
bool CDarkSendPool::SignatureValid(const CScript& newSig, const CTxIn& newVin){
    CTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int found = -1;
    CScript sigPubKey = CScript();
    unsigned int i = 0;

    BOOST_FOREACH(CDarkSendEntry e, entries) {
        BOOST_FOREACH(const CTxOut out, e.vout)
            txNew.vout.push_back(out);

        BOOST_FOREACH(const CDarkSendEntryVin s, e.sev){
            txNew.vin.push_back(s.vin);

            if(s.vin == newVin){
                found = i;
                sigPubKey = s.vin.prevPubKey;
            }
            i++;
        }
    }

    if(found >= 0){ //might have to do this one input at a time?
        int n = found;
        txNew.vin[n].scriptSig = newSig;
        if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Sign with sig %s\n", newSig.ToString().substr(0,24).c_str());
        if (!VerifyScript(txNew.vin[n].scriptSig, sigPubKey, txNew, n, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, 0)){
            if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Signing - Error signing input %u\n", n);
            return false;
        }
    }

    if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Signing - Succesfully signed input\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CDarkSendPool::IsCollateralValid(const CTransaction& txCollateral){
    if(txCollateral.vout.size() < 1) return false;

    int64 nValueIn = 0;
    int64 nValueOut = 0;
    bool missingTx = false;

    CTransaction tx;
    BOOST_FOREACH(const CTxOut o, txCollateral.vout)
        nValueOut += o.nValue;

    BOOST_FOREACH(const CTxIn i, txCollateral.vin){
        CTransaction tx2;
        uint256 hash;
        if(GetTransaction(i.prevout.hash, tx2, hash, true)){
            if(tx2.vout.size() > i.prevout.n) {
                nValueIn += tx2.vout[i.prevout.n].nValue;    
            }
        } else{
            missingTx = true;
        }
    }

    if(missingTx){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - Unknown inputs in collateral transaction - %s\n", txCollateral.ToString().c_str());
        return false; 
    }

    //collateral transactions are required to pay out DARKSEND_COLLATERAL as a fee to the miners
    if(nValueIn-nValueOut < DARKSEND_COLLATERAL) {
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - did not include enough fees in transaction %"PRI64d"\n%s\n", nValueOut-nValueIn, txCollateral.ToString().c_str());
        return false;
    }

    LogPrintf("CDarkSendPool::IsCollateralValid %s\n", txCollateral.ToString().c_str());

    CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);
    if (!wtxCollateral.IsAcceptable(true, false)){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - didn't pass IsAcceptable\n");
        return false;
    }

    return true;
}


// 
// Add a clients transaction to the pool
//
bool CDarkSendPool::AddEntry(const std::vector<CTxIn>& newInput, const int64& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error){
    if (!fMasterNode) return false;

    BOOST_FOREACH(CTxIn in, newInput) {
        if (in.prevout.IsNull() || nAmount < 0) {
            if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - input not valid!\n");
            error = "input not valid";
            sessionUsers--;
            return false;
        }
    }

    if (!IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - collateral not valid!\n");
        error = "collateral not valid";
        sessionUsers--;
        return false;
    }

    if((int)entries.size() >= GetMaxPoolTransactions()){
        if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - entries is full!\n");   
        error = "entries is full";
        sessionUsers--;
        return false;
    }

    BOOST_FOREACH(CTxIn in, newInput) {
        LogPrintf("looking for vin -- %s\n", in.ToString().c_str());
        BOOST_FOREACH(const CDarkSendEntry v, entries) {
            BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
                if(s.vin == in) {
                    if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - found in vin\n"); 
                    error = "already have that vin";
                    sessionUsers--;
                    return false;
                }
            }
        }
    }

    if(state == POOL_STATUS_ACCEPTING_ENTRIES) {
        CDarkSendEntry v;
        v.Add(newInput, nAmount, txCollateral, newOutput);
        entries.push_back(v);

        LogPrintf("CDarkSendPool::AddEntry -- adding %s\n", newInput[0].ToString().c_str());
        error = "";

        return true;
    }

    if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - can't accept new entry, wrong state!\n");
    error = "wrong state";
    sessionUsers--;
    return false;
}

bool CDarkSendPool::AddScriptSig(const CTxIn& newVin){
    if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- new sig  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
    
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
            if(s.vin.scriptSig == newVin.scriptSig) {
                LogPrintf("CDarkSendPool::AddScriptSig - already exists \n");
                return false;
            }
        }
    }

    if(!SignatureValid(newVin.scriptSig, newVin)){
        if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig - Invalid Sig\n");
        return false;
    }

    if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- sig %s\n", newVin.ToString().c_str());

    if(state == POOL_STATUS_SIGNING) {
        BOOST_FOREACH(CTxIn& vin, finalTransaction.vin){
            if(newVin.prevout == vin.prevout && vin.nSequence == newVin.nSequence){
                vin.scriptSig = newVin.scriptSig;
                vin.prevPubKey = newVin.prevPubKey;
                if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- adding to finalTransaction  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
            }
        }
        for(unsigned int i = 0; i < entries.size(); i++){
            if(entries[i].AddSig(newVin)){
                if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- adding  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
                return true;
            }
        }
    }

    LogPrintf("CDarkSendPool::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// check to make sure everything is signed
bool CDarkSendPool::SignaturesComplete(){
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
            if(!s.isSigSet) return false;
        }
    }
    return true;
}

//
// Execute a darksend denomination via a masternode.
// This is only ran from clients
// 
void CDarkSendPool::SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64 amount){
    if(darkSendPool.txCollateral == CTransaction()){
        LogPrintf ("CDarksendPool:SendDarksendDenominate() - Darksend collateral not set");
        return;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn in, txCollateral.vin)
        lockedCoins.push_back(in);
    
    BOOST_FOREACH(CTxIn in, vin)
        lockedCoins.push_back(in);

    //BOOST_FOREACH(CTxOut o, vout)
    //    LogPrintf(" vout - %s\n", o.ToString().c_str());


    // we should already be connected to a masternode
    if(!sessionFoundMasternode){
        LogPrintf("CDarkSendPool::SendDarksendDenominate() - No masternode has been selected yet.\n");
        UnlockCoins();
        SetNull(true);
        return;
    }

    if (!CheckDiskSpace())
        return;

    if(fMasterNode) {
        LogPrintf("CDarkSendPool::SendDarksendDenominate() - DarkSend from a masternode is not supported currently.\n");
        return;
    }

    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

    LogPrintf("CDarkSendPool::SendDarksendDenominate() - Added transaction to pool.\n");

    ClearLastMessage();

    //check it against the memory pool to make sure it's valid
    {
        int64 nValueOut = 0;

        CValidationState state;
        CTransaction tx;

        BOOST_FOREACH(const CTxOut o, vout){
            nValueOut += o.nValue;
            tx.vout.push_back(o);
        }

        BOOST_FOREACH(const CTxIn i, vin){
            tx.vin.push_back(i);

            LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());                
        }


        bool missing = false;
        if (!tx.IsAcceptable(state, true, false, &missing, false)){ //AcceptableInputs(state, true)){
            LogPrintf("dsi -- transactione not valid! %s \n", tx.ToString().c_str());
            return;
        }
    }

    // store our entry for later use
    CDarkSendEntry e;
    e.Add(vin, amount, txCollateral, vout);
    myEntries.push_back(e);

    // relay our entry to the master node
    RelayDarkSendIn(vin, amount, txCollateral, vout);
    Check();
}

// Incoming message from masternode updating the progress of darksend
//    newAccepted:  -1 mean's it'n not a "transaction accepted/not accepted" message, just a standard update
//                  0 means transaction was not accepted
//                  1 means transaction was accepted

bool CDarkSendPool::StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID){
    if(fMasterNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    UpdateState(newState);
    entriesCount = newEntriesCount;

    if(newAccepted != -1) {
        lastEntryAccepted = newAccepted;
        countEntriesAccepted += newAccepted;
        if(newAccepted == 0){
            UpdateState(POOL_STATUS_ERROR);
            lastMessage = error;
        }

        if(newAccepted == 1) {
            sessionID = newSessionID;
            LogPrintf("CDarkSendPool::StatusUpdate - set sessionID to %d\n", sessionID);
            sessionFoundMasternode = true;
        }
    }

    if(newState == POOL_STATUS_ACCEPTING_ENTRIES){
        if(newAccepted == 1){
            LogPrintf("CDarkSendPool::StatusUpdate - entry accepted! \n");
            sessionFoundMasternode = true;
            //wait for other users. Masternode will report when ready
            UpdateState(POOL_STATUS_QUEUE);
        } else if (newAccepted == 0 && sessionID == 0 && !sessionFoundMasternode) {
            LogPrintf("CDarkSendPool::StatusUpdate - entry not accepted by masternode \n");
            UnlockCoins();
            DoAutomaticDenominating(); //try another masternode
        }
        if(sessionFoundMasternode) return true;
    }

    return true;
}

// 
// After we receive the finalized transaction from the masternode, we must 
// check it to make sure it's what we want, then sign it if we agree. 
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CDarkSendPool::SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node){
    if(fDebug) LogPrintf("CDarkSendPool::AddFinalTransaction - Got Finalized Transaction\n");

    if(!finalTransaction.vin.empty()){
        LogPrintf("CDarkSendPool::AddFinalTransaction - Rejected Final Transaction!\n");
        return false;
    }

    finalTransaction = finalTransactionNew;
    LogPrintf("CDarkSendPool::SignFinalTransaction %s\n", finalTransaction.ToString().c_str());
    
    vector<CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(const CDarkSendEntry e, myEntries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, e.sev) {
            /* Sign my transaction and all outputs */
            int mine = -1;
            CScript prevPubKey = CScript();
            CTxIn vin = CTxIn();
            
            for(unsigned int i = 0; i < finalTransaction.vin.size(); i++){
                if(finalTransaction.vin[i] == s.vin){
                    mine = i;
                    prevPubKey = s.vin.prevPubKey;
                    vin = s.vin;
                }
            }

            if(mine >= 0){ //might have to do this one input at a time?
                int foundOutputs = 0;
                int64 nValue1 = 0;
                int64 nValue2 = 0;
                
                for(unsigned int i = 0; i < finalTransaction.vout.size(); i++){
                    BOOST_FOREACH(const CTxOut o, e.vout) {
                        if(finalTransaction.vout[i] == o){
                            foundOutputs++;
                            nValue1 += finalTransaction.vout[i].nValue;
                        }
                    }
                }
                
                BOOST_FOREACH(const CTxOut o, e.vout)
                    nValue2 += o.nValue;

                int targetOuputs = e.vout.size();
                if(foundOutputs < targetOuputs || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's 
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CDarkSendPool::Sign - My entries are not correct! Refusing to sign. %d entries %d target. \n", foundOutputs, targetOuputs);
                    return false;
                }

                if(fDebug) LogPrintf("CDarkSendPool::Sign - Signing my input %i\n", mine);
                if(!SignSignature(*pwalletMain, prevPubKey, finalTransaction, mine, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    if(fDebug) LogPrintf("CDarkSendPool::Sign - Unable to sign my own transaction! \n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalTransaction.vin[mine]);
                if(fDebug) LogPrintf(" -- dss %d %d %s\n", mine, (int)sigs.size(), finalTransaction.vin[mine].scriptSig.ToString().c_str());
            }
            
        }
        
        if(fDebug) LogPrintf("CDarkSendPool::Sign - txNew:\n%s", finalTransaction.ToString().c_str());
    }

    // push all of our signatures to the masternode
    if(sigs.size() > 0 && node != NULL)
        node->PushMessage("dss", sigs);

    return true;
}

bool CDarkSendPool::GetBlockHash(uint256& hash, int nBlockHeight)
{
    if(unitTest){
        hash.SetHex("00000000001432b4910722303bff579d0445fa23325bdc34538bdb226718ba79");
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0) { return false; }

    //printf(" nBlockHeight2 %"PRI64u" %"PRI64u"\n", nBlockHeight, pindexBest->nHeight+1);
   
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(BlockReading->nHeight == nBlockHeight) {
            hash = BlockReading->GetBlockHash();
            return true;
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;    
}

//Get the last hash that matches the modulus given. Processed in reverse order
bool CDarkSendPool::GetLastValidBlockHash(uint256& hash, int mod, int nBlockHeight)
{
    if(unitTest){
        hash.SetHex("00000000001432b4910722303bff579d0445fa23325bdc34538bdb226718ba79");
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0) { return false; }

    int nBlocksAgo = 0;
    if(nBlockHeight > 0) nBlocksAgo = (pindexBest->nHeight+1)-nBlockHeight;
    assert(nBlocksAgo >= 0);
    
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(BlockReading->nHeight % mod == 0) {
            if(n >= nBlocksAgo){
                hash = BlockReading->GetBlockHash();
                return true;
            }
            n++;
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;    
}

void CDarkSendPool::NewBlock()
{
    if(fDebug) LogPrintf("CDarkSendPool::NewBlock \n");

    if(IsInitialBlockDownload()) return;
    
    masternodePayments.ProcessBlock(pindexBest->nHeight+10);

    if(!fEnableDarksend) return;

    if(!fMasterNode){
        //denominate all non-denominated inputs every 25 minutes.
        if(pindexBest->nHeight % 10 == 0) UnlockCoins();
        ProcessMasternodeConnections();
    }
}

// Darksend transaction was completed (failed or successed)
void CDarkSendPool::CompletedTransaction(bool error, std::string lastMessageNew)
{
    if(fMasterNode) return;

    if(error){
        LogPrintf("CompletedTransaction -- error \n");
        UpdateState(POOL_STATUS_ERROR);
    } else {
        LogPrintf("CompletedTransaction -- success \n");
        UpdateState(POOL_STATUS_SUCCESS);

        myEntries.clear();

        // To avoid race conditions, we'll only let DS run once per block
        cachedLastSuccess = nBestHeight;
        splitUpInARow = 0;
    }
    lastMessage = lastMessageNew;

    completedTransaction = true;
    Check();
    UnlockCoins();
}

void CDarkSendPool::ClearLastMessage()
{
    lastMessage = "";
}

// 
// Passively run Darksend in the background to anonymize funds based on the given configuration.
//
// This does NOT run by default for daemons, only for QT. 
//
bool CDarkSendPool::DoAutomaticDenominating(bool fDryRun, bool ready)
{
    if(fMasterNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    if(nBestHeight-cachedLastSuccess < minBlockSpacing) {
        LogPrintf("CDarkSendPool::DoAutomaticDenominating - Last successful darksend was too recent\n");
        return false;
    }
    if(!fEnableDarksend) {
        LogPrintf("CDarkSendPool::DoAutomaticDenominating - Darksend is disabled\n");
        return false; 
    }

    if (!fDryRun && pwalletMain->IsLocked()){
        return false;
    }

    if(darkSendPool.GetState() != POOL_STATUS_ERROR && darkSendPool.GetState() != POOL_STATUS_SUCCESS){
        if(darkSendPool.GetMyTransactionCount() > 0){
            return true;
        }
    } 

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    int64 nValueMin = 0.01*COIN;
    int64 nValueMax = 999*COIN;
    int64 nValueIn = 0;
    int minRounds = -2; //non denominated funds are rounds of less than 0
    int maxAmount = 1000;
    bool hasFeeInput = false;

    // if we have more denominated funds (of any maturity) than the nAnonymizeDarkcoinAmount, we should use use those
    if(pwalletMain->GetDenominatedBalance(true) >= nAnonymizeDarkcoinAmount*COIN) {
        minRounds = 0;
    }
    //if we're set to less than a thousand, don't submit for than that to the pool
    if(nAnonymizeDarkcoinAmount < 1000) maxAmount = nAnonymizeDarkcoinAmount;

    int64 balanceNeedsAnonymized = pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance();
    if(balanceNeedsAnonymized > maxAmount*COIN) balanceNeedsAnonymized= maxAmount*COIN;
    if(balanceNeedsAnonymized < COIN*2.5){
        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating \n");
        return false;
    }

    // if the balance is more the pool max, take the pool max
    if(balanceNeedsAnonymized > nValueMax) balanceNeedsAnonymized = nValueMax;

    // select coins that should be given to the pool
    if (!pwalletMain->SelectCoinsDark(nValueMin, maxAmount*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
    {
        nValueIn = 0;
        vCoins.clear();

        // look for inputs larger than the max amount, if we find anything we need to split it up
        if (pwalletMain->SelectCoinsDark(maxAmount*COIN, 9999999*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
        {
            if(!fDryRun) SplitUpMoney();
            return true;
        }

        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating (2)\n");
        return false;
    }

    // the darksend pool can only take 1.1DRK minimum
    if(nValueIn < COIN*1.1){
        //simply look for non-denominated coins
        if (pwalletMain->SelectCoinsDark(maxAmount*COIN, 9999999*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
        {
            if(!fDryRun) SplitUpMoney();
            return true;
        }

        LogPrintf("DoAutomaticDenominating : Too little to denominate (must have 1.1DRK) \n");
        return false;
    }

    //check to see if we have the fee sized inputs, it requires these
    if(!pwalletMain->HasDarksendFeeInputs()){
        if(!fDryRun) SplitUpMoney(true);
        return true;
    }

    if(fDryRun) return true;

    // initial phase, find a masternode
    if(!sessionFoundMasternode){
        int nUseQueue = rand()%100;

        sessionTotalValue = pwalletMain->GetTotalValue(vCoins);
        //randomize the amounts we mix
        // if we have minRounds set, or if our non-demon is less than 5% of denom coins 
        if(minRounds == 0 || 
            pwalletMain->GetDenominatedBalance(true) * 0.05 > pwalletMain->GetDenominatedBalance(false)) {
            for(int a = 0; a < 10; a++){ //try 10 amounts and see if we match a queue
                int r = (rand()%(maxAmount-(nValueMin/COIN)))+(nValueMin/COIN);

                vCoins.clear();
                nValueIn = 0;
                if (pwalletMain->SelectCoinsDark(nValueMin, r*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput)){
                    sessionTotalValue = pwalletMain->GetTotalValue(vCoins);

                    // if it's in the queue, take it
                    if(nUseQueue > 33 && vecDarksendQueue.size() > 0){
                        BOOST_FOREACH(CDarksendQueue& dsq, vecDarksendQueue){
                            CService addr;
                            if(dsq.time == 0) continue;
                            if(!dsq.GetAddress(addr)) continue;

                            if(dsq.nDenom == GetDenominationsByAmount(sessionTotalValue)) {
                                break;
                            }
                        }
                    } else {
                        break;
                    }

                }
            }
        }
        if(sessionTotalValue > maxAmount*COIN) sessionTotalValue = maxAmount*COIN;

        double fDarkcoinSubmitted = sessionTotalValue / COIN;

        LogPrintf("Submiting Darksend for %f DRK\n", fDarkcoinSubmitted);

        if(pwalletMain->GetDenominatedBalance(true, true) > 0){ //get denominated unconfirmed inputs 
            LogPrintf("DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
            return false;
        }

        //don't use the queues all of the time for mixing
        if(nUseQueue > 33){

            // Look through the queues and see if anything matches
            BOOST_FOREACH(CDarksendQueue& dsq, vecDarksendQueue){
                CService addr;
                if(dsq.time == 0) continue;
                if(!dsq.GetAddress(addr)) continue;

                //don't reuse masternodes
                BOOST_FOREACH(CTxIn usedVin, vecMasternodesUsed){
                    if(dsq.vin == usedVin) {
                        continue;
                    }
                }

                // If we don't match the denominations, we don't want to submit our inputs
                if(dsq.nDenom != GetDenominationsByAmount(sessionTotalValue)) {
                    if(fDebug) LogPrintf(" dsq.nDenom != GetDenominationsByAmount %d %d \n", dsq.nDenom, GetDenominationsByAmount(sessionTotalValue));
                    continue;
                }
                dsq.time = 0; //remove node

                // connect to masternode and submit the queue request
                if(ConnectNode((CAddress)addr, NULL, true)){
                    submittedToMasternode = addr;
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if(submittedToMasternode != pnode->addr) continue;
                    
                        std::string strReason;
                        if(txCollateral == CTransaction()){
                            if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                                LogPrintf("DoAutomaticDenominating -- dsa error:%s\n", strReason.c_str());
                                return false; 
                            }
                        }
                    
                        vecMasternodesUsed.push_back(dsq.vin);
                        sessionDenom = GetDenominationsByAmount(sessionTotalValue);
                        pnode->PushMessage("dsa", sessionDenom, txCollateral);
                        LogPrintf("DoAutomaticDenominating --- connected (from queue), sending dsa for %d %d - %s\n", sessionDenom, GetDenominationsByAmount(sessionTotalValue), pnode->addr.ToString().c_str());
                        return true;
                    }
                } else {
                    LogPrintf("DoAutomaticDenominating --- error connecting \n");
                    return DoAutomaticDenominating();
                }
            }
        }

        // otherwise, try one randomly
        if(sessionTries++ < 10){
            //pick a random masternode to use
            int max_value = darkSendMasterNodes.size();
            if(max_value <= 0) return false;
            int i = (rand() % max_value);

            //don't reuse masternodes
            BOOST_FOREACH(CTxIn usedVin, vecMasternodesUsed) {
                if(darkSendMasterNodes[i].vin == usedVin){
                    return DoAutomaticDenominating();
                }
            }

            if(darkSendMasterNodes[i].nLastDsq != 0 && 
                darkSendMasterNodes[i].nLastDsq + (int)darkSendMasterNodes.size()/5 > darkSendPool.nDsqCount){
                return DoAutomaticDenominating();
            }

            lastTimeChanged = GetTimeMillis();
            LogPrintf("DoAutomaticDenominating -- attempt %d connection to masternode %s\n", sessionTries, darkSendMasterNodes[i].addr.ToString().c_str());
            if(ConnectNode((CAddress)darkSendMasterNodes[i].addr, NULL, true)){
                submittedToMasternode = darkSendMasterNodes[i].addr;
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if(darkSendMasterNodes[i].addr != pnode->addr) continue;

                    std::string strReason;
                    if(txCollateral == CTransaction()){
                        if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                            LogPrintf("DoAutomaticDenominating -- dsa error:%s\n", strReason.c_str());
                            return false; 
                        }
                    }

                    vecMasternodesUsed.push_back(darkSendMasterNodes[i].vin);
                    sessionDenom = GetDenominationsByAmount(sessionTotalValue);
                    pnode->PushMessage("dsa", sessionDenom, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected, sending dsa for %d - denom %d\n", sessionDenom, GetDenominationsByAmount(sessionTotalValue));
                    return true;
                }
            } else {
                LogPrintf("DoAutomaticDenominating --- error connecting \n");
                return DoAutomaticDenominating();
            }
        } else {
            return false;
        }
    }

    if(!ready) return true;

    if(sessionDenom == 0) return true;

    // Submit transaction to the pool if we get here, use sessionDenom so we use the same amount of money
    std::string strError = pwalletMain->PrepareDarksendDenominate(minRounds, sessionTotalValue);
    LogPrintf("DoAutomaticDenominating : Running darksend denominate. Return '%s'\n", strError.c_str());
   
    if(strError == "") return true;

    LogPrintf("DoAutomaticDenominating : Error running denominate, %s\n", strError.c_str());
    return false;
}

bool CDarkSendPool::SendRandomPaymentToSelf()
{
    int64 nBalance = pwalletMain->GetBalance();
    int64 nPayment = (nBalance*0.35) + (rand() % nBalance);

    if(nPayment > nBalance) nPayment = nBalance-(0.1*COIN);

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange.SetDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64 nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64> > vecSend;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, nPayment));

    CCoinControl *coinControl=NULL;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey, nFeeRet, strFail, coinControl, ONLY_DENOMINATED);
    if(!success){
        LogPrintf("SendRandomPaymentToSelf: Error - %s\n", strFail.c_str());
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SendRandomPaymentToSelf Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    return true;
}

// Split up large inputs or create fee sized inputs
bool CDarkSendPool::SplitUpMoney(bool justCollateral)
{
    if((nBestHeight - lastSplitUpBlock) < 10){
        LogPrintf("SplitUpMoney - Too soon to split up again\n");
        return false;
    }

    if(splitUpInARow >= 2){
        LogPrintf("Error: Darksend SplitUpMoney was called multiple times in a row. This should not happen. Please submit a detailed explanation of the steps it took to create this error and submit to evan@darkcoin.io. \n");
        fEnableDarksend = false;
        return false;
    }

    int64 nTotalBalance = pwalletMain->GetDenominatedBalance(false);
    if(justCollateral && nTotalBalance > 1*COIN) nTotalBalance = 1*COIN;
    int64 nTotalOut = 0;
    lastSplitUpBlock = nBestHeight;

    LogPrintf("DoAutomaticDenominating: Split up large input (justCollateral %d):\n", justCollateral);
    LogPrintf(" -- nTotalBalance %"PRI64d"\n", nTotalBalance);
    LogPrintf(" -- denom %"PRI64d" \n", pwalletMain->GetDenominatedBalance(false));

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange.SetDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64 nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64> > vecSend;

    int64 a = 1*COIN;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, (DARKSEND_COLLATERAL*5)+DARKSEND_FEE));
    nTotalOut += (DARKSEND_COLLATERAL*5)+DARKSEND_FEE;
    vecSend.push_back(make_pair(scriptChange, (DARKSEND_COLLATERAL*5)+DARKSEND_FEE));
    nTotalOut += (DARKSEND_COLLATERAL*5)+DARKSEND_FEE;

    // ****** Add outputs in bases of two from 1 darkcoin *** /
    if(!justCollateral){
        bool continuing = true;

        while(continuing){
            if(nTotalOut + a < nTotalBalance){
                //LogPrintf(" nTotalOut %"PRI64d", added %"PRI64d"\n", nTotalOut, a);

                vecSend.push_back(make_pair(scriptChange, a));
                nTotalOut += a;
            } else {
                continuing = false;
            }

            a = a * 2;
        }
    } 

    if((justCollateral && nTotalOut <= 0.1*COIN) || vecSend.size() < 1) {
        LogPrintf("SplitUpMoney: Not enough outputs to make a transaction\n");
        return false;
    }
    if((!justCollateral && nTotalOut <= 1.1*COIN) || vecSend.size() < 1){
        LogPrintf("SplitUpMoney: Not enough outputs to make a transaction\n");
        return false;
    }

    CCoinControl *coinControl=NULL;
	bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey,
			nFeeRet, strFail, coinControl, justCollateral ? ALL_COINS : ONLY_NONDENOMINATED);
    if(!success){
        LogPrintf("SplitUpMoney: Error - %s\n", strFail.c_str());
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SplitUpMoney Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    splitUpInARow++;
    return true;
}

bool CDarkSendPool::IsCompatibleWithEntries(std::vector<CTxOut> vout)
{
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        LogPrintf(" IsCompatibleWithEntries %d %d\n", GetDenominations(vout), GetDenominations(v.vout));
/*
        BOOST_FOREACH(CTxOut o1, vout)
            LogPrintf(" vout 1 - %s\n", o1.ToString().c_str());

        BOOST_FOREACH(CTxOut o2, v.vout)
            LogPrintf(" vout 2 - %s\n", o2.ToString().c_str());
*/
        if(GetDenominations(vout) != GetDenominations(v.vout)) return false;
    }

    return true;
}

bool CDarkSendPool::IsCompatibleWithSession(int64 nDenom, CTransaction txCollateral, std::string& strReason)
{
    LogPrintf("CDarkSendPool::IsCompatibleWithSession - sessionDenom %d sessionUsers %d\n", sessionDenom, sessionUsers);

    if (!unitTest && !IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCompatibleWithSession - collateral not valid!\n");
        strReason = "collateral not valid";
        return false;
    }

    if(sessionUsers < 0) sessionUsers = 0;
    
    if(sessionUsers == 0) {
        sessionDenom = nDenom;
        sessionUsers++;
        lastTimeChanged = GetTimeMillis();
        entries.clear();

        if(!unitTest){
            //broadcast that I'm accepting entries, only if it's the first entry though
            CDarksendQueue dsq;
            dsq.nDenom = nDenom;
            dsq.vin = activeMasternode.vinMasternode;
            dsq.time = GetTime();
            dsq.Sign();
            dsq.Relay();
        }

        UpdateState(POOL_STATUS_QUEUE);
        vecSessionCollateral.push_back(txCollateral);
        return true;
    }

    if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE) || sessionUsers >= GetMaxPoolTransactions()){
        if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE)) strReason = "incompatible mode";
        if(sessionUsers >= GetMaxPoolTransactions()) strReason = "masternode queue is full";
        LogPrintf("CDarkSendPool::IsCompatibleWithSession - incompatible mode, return false %d %d\n", state != POOL_STATUS_ACCEPTING_ENTRIES, sessionUsers >= GetMaxPoolTransactions());
        return false;
    }

    if(nDenom != sessionDenom) {
        strReason = "no matching denominations found for mixing";
        return false;
    }

    LogPrintf("CDarkSendPool::IsCompatibleWithSession - compatible\n");

    sessionUsers++;
    lastTimeChanged = GetTimeMillis();
    vecSessionCollateral.push_back(txCollateral);

    return true;
}

// return a bitshifted integer representing the denominations in this list
int CDarkSendPool::GetDenominations(const std::vector<CTxOut>& vout){
    std::vector<pair<int64, int> > denomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(int64 d, darkSendDenominations)
        denomUsed.push_back(make_pair(d, 0));

    // look for denominations and update uses to 1
    bool found_non_denom = false;
    BOOST_FOREACH(CTxOut out, vout){
        bool found = false;
        BOOST_FOREACH (PAIRTYPE(int64, int)& s, denomUsed){
            if (out.nValue == s.first){
                s.second = 1;
                found = true;
            }
        }
        if(!found) found_non_denom = true;
    }

    //if other inputs are in here, flip the last bit
    if(found_non_denom){
        denomUsed.push_back(make_pair(0, 1));
    }

    int denom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on.
    // then move to the next
    BOOST_FOREACH (PAIRTYPE(int64, int)& s, denomUsed)
        denom |= s.second << c++;

    // Function returns as follows:
    //
    // bit 0 - 500DRK+1 ( bit on if present )
    // bit 1 - 100DRK+1 
    // bit 2 - 10DRK+1 
    // bit 3 - 1DRK+1 
    // bit 4 - fee
    // bit 5 - other sizes

    return denom;
}

// calculate the outputs from a given amount of darkcoin and 
// return the bitshifted integer to represent them
int CDarkSendPool::GetDenominationsByAmount(int64 nAmount){
    CScript e = CScript();
    int64 nValueLeft = nAmount;

    std::vector<CTxOut> vout1;
    BOOST_FOREACH(int64 v, darkSendDenominations){
        int nOutputs = 0;
        while(nValueLeft - v >= 0 && nOutputs <= 100) {
            CTxOut o(v, e);
            vout1.push_back(o);
            nValueLeft -= v;
            nOutputs++;
        }
    }

    //add non-denom left overs as change
    if(nValueLeft > 0){
        CTxOut o(nValueLeft, e);
        vout1.push_back(o);
    }

    return GetDenominations(vout1);
}

bool CDarkSendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey){
    CScript payee2;
    payee2.SetDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;
    if(GetTransaction(vin.prevout.hash, txVin, hash, true)){
        BOOST_FOREACH(CTxOut out, txVin.vout){
            if(out.nValue == 1000*COIN){
                if(out.scriptPubKey == payee2) return true;
            }
        }
    }

    return false;
}

bool CDarkSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood) {
        errorMessage = "Invalid private key";
        return false;
    }     

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CDarkSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Sign failed";
        return false;
    }

    return true;
}

bool CDarkSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Error recovering pubkey";
        return false;
    }

    return (pubkey2.GetID() == pubkey.GetID());
}

bool CDarksendQueue::Sign()
{
    if(!fMasterNode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready); 

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        exit(0);
    }

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchSig, key2)) {
        LogPrintf("CDarksendQueue():Relay - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage)) {
        LogPrintf("CDarksendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CDarksendQueue::Relay()
{

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        // always relay to everyone
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CDarksendQueue::CheckSignature()
{
    BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes) {

        if(mn.vin == vin) {
            std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready); 

            std::string errorMessage = "";
            if(!darkSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                return error("Got bad masternode address signature %s \n", vin.ToString().c_str());
            }

            return true;
        }
    }

    return false;
}


void ThreadCheckDarkSendPool()
{
    // Make this thread recognisable as the wallet flushing thread
    RenameThread("bitcoin-darksend");

    unsigned int c = 0;
    while (true)
    {
        MilliSleep(1000);
        //LogPrintf("ThreadCheckDarkSendPool::check timeout\n");
        darkSendPool.CheckTimeout();
        
        if(c % 60 == 0){
            vector<CMasterNode>::iterator it = darkSendMasterNodes.begin();
            while(it != darkSendMasterNodes.end()){
                (*it).Check();
                if((*it).enabled == 4 || (*it).enabled == 3){
                    LogPrintf("Removing inactive masternode %s\n", (*it).addr.ToString().c_str());
                    it = darkSendMasterNodes.erase(it);
                } else {
                    ++it;
                }
            }

            masternodePayments.CleanPaymentList();
        }


        //try to sync the masternode list and payment list every 20 seconds
        if(c % 5 == 0 && RequestedMasterNodeList <= 2){
            bool fIsInitialDownload = IsInitialBlockDownload();
            if(!fIsInitialDownload) {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if (pnode->nVersion >= darkSendPool.MIN_PEER_PROTO_VERSION) {

                        //keep track of who we've asked for the list
                        if(pnode->HasFulfilledRequest("mnsync")) continue;
                        pnode->FulfilledRequest("mnsync");

                        LogPrintf("Successfully synced, asking for Masternode list and payment list\n");
        
                        pnode->PushMessage("dseg", CTxIn()); //request full mn list
                        pnode->PushMessage("mnget"); //sync payees
                        RequestedMasterNodeList++;
                    }
                }
            }
        }


        if(c == MASTERNODE_PING_SECONDS){
            activeMasternode.RegisterAsMasterNode(false);
            c = 0;
        }

        if(c % 60 == 0){
            //if we've used 1/5 of the masternode list, then clear the list.
            if((int)vecMasternodesUsed.size() > (int)darkSendMasterNodes.size() / 5) 
                vecMasternodesUsed.clear();

        }

        //clear this every 3 hours
        if(c % 60*60*3 == 0) vecMasternodeAskedFor.clear();

        //auto denom every 2.5 minutes (liquidity provides try less often)
        if(c % 60*(nLiquidityProvider+1) == 0){
            if(nLiquidityProvider!=0){
                int nRand = rand() % (101+nLiquidityProvider);
                //about 1/100 chance of starting over after 4 rounds. 
                if(nRand == 50+nLiquidityProvider && pwalletMain->GetAverageAnonymizedRounds() > 8){
                    darkSendPool.SendRandomPaymentToSelf();
                    int nLeftToAnon = ((pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance())/COIN)-3;
                    if(nLeftToAnon > 999) nLeftToAnon = 999;
                    nAnonymizeDarkcoinAmount = (rand() % nLeftToAnon)+3;
                } else {
                    darkSendPool.DoAutomaticDenominating();
                }
            } else {
                darkSendPool.DoAutomaticDenominating();
            }
        }

        c++;
    }
}
