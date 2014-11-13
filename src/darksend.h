
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DARKSEND_H
#define DARKSEND_H

#include "core.h"
#include "masternode.h"
#include "main.h"

class CTxIn;
class CDarkSendPool;
class CDarkSendSigner;
class CMasterNodeVote;
class CBitcoinAddress;
class CDarksendQueue;

#define POOL_MAX_TRANSACTIONS                  3 // wait for X transactions to merge and publish
#define POOL_STATUS_UNKNOWN                    0 // waiting for update
#define POOL_STATUS_IDLE                       1 // waiting for update
#define POOL_STATUS_QUEUE                      2 // waiting in a queue
#define POOL_STATUS_ACCEPTING_ENTRIES          3 // accepting entries
#define POOL_STATUS_FINALIZE_TRANSACTION       4 // master node will broadcast what it accepted
#define POOL_STATUS_SIGNING                    5 // check inputs/outputs, sign final tx
#define POOL_STATUS_TRANSMISSION               6 // transmit transaction
#define POOL_STATUS_ERROR                      7 // error
#define POOL_STATUS_SUCCESS                    8 // success

// status update message constants
#define MASTERNODE_ACCEPTED                    1
#define MASTERNODE_REJECTED                    0
#define MASTERNODE_RESET                       -1

extern CDarkSendPool darkSendPool;
extern CDarkSendSigner darkSendSigner;
extern std::vector<int64> darkSendDenominations;
extern std::vector<CDarksendQueue> vecDarksendQueue;
extern std::string strMasterNodePrivKey;

static const int64 DARKSEND_COLLATERAL = (0.1*COIN);
static const int64 DARKSEND_FEE = 0.0125*COIN;

//specific messages for the Darksend protocol
void ProcessMessageDarksend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

// get the darksend chain depth for a given input
int GetInputDarksendRounds(CTxIn in, int rounds=0);


// An input in the darksend pool
class CDarkSendEntryVin
{
public:
    bool isSigSet;
    CTxIn vin;

    CDarkSendEntryVin()
    {
        isSigSet = false;
        vin = CTxIn();
    }
};

// A clients transaction in the darksend pool
class CDarkSendEntry
{
public:
    bool isSet;
    std::vector<CDarkSendEntryVin> sev;
    int64 amount;
    CTransaction collateral;
    std::vector<CTxOut> vout;
    CTransaction txSupporting;
    int64 addedTime;

    CDarkSendEntry()
    {
        isSet = false;
        collateral = CTransaction();
        amount = 0;
    }

    bool Add(const std::vector<CTxIn> vinIn, int64 amountIn, const CTransaction collateralIn, const std::vector<CTxOut> voutIn)
    {
        if(isSet){return false;}

        BOOST_FOREACH(const CTxIn v, vinIn) {
            CDarkSendEntryVin s = CDarkSendEntryVin();
            s.vin = v;
            sev.push_back(s);
        }
        vout = voutIn;
        amount = amountIn;
        collateral = collateralIn;
        isSet = true;
        addedTime = GetTime();
        
        return true;
    }

    bool AddSig(const CTxIn& vin)
    {
        BOOST_FOREACH(CDarkSendEntryVin& s, sev) {
            if(s.vin.prevout == vin.prevout && s.vin.nSequence == vin.nSequence){
                if(s.isSigSet){return false;}
                s.vin.scriptSig = vin.scriptSig;
                s.vin.prevPubKey = vin.prevPubKey;
                s.isSigSet = true;
                
                return true;
            }
        }

        return false;
    }

    bool IsExpired()
    {
        return (GetTime() - addedTime) > 120;// 120 seconds
    }
};

// 
// A currently inprogress darksend merge and denomination information
//
class CDarksendQueue
{
public:
    CTxIn vin;
    int64 time;
    int nDenom;
    bool ready; //ready for submit
    std::vector<unsigned char> vchSig;

    CDarksendQueue()
    {
        nDenom = 0;
        vin = CTxIn();
        time = 0;   
        vchSig.clear();
        ready = false;
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(nDenom);
        READWRITE(vin);
        READWRITE(time);
        READWRITE(ready);
        READWRITE(vchSig);
    )

    int GetAddress(CService &addr)
    {
        BOOST_FOREACH(CMasterNode mn, darkSendMasterNodes) {
            if(mn.vin == vin){
                addr = mn.addr;
                return true;
            }
        }
        return false;
    }

    bool Sign();
    bool Relay();

    bool IsExpired()
    {
        return (GetTime() - time) > 120;// 120 seconds
    }

    bool CheckSignature();

};

//
// Helper object for signing and checking signatures
//
class CDarkSendSigner
{
public:
    bool IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey);
    bool SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey);
    bool SignMessage(std::string strMessage, std::string& errorMessage, std::vector<unsigned char>& vchSig, CKey key);
    bool VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage);
};

class CDarksendSession
{

};

//
// Used to keep track of current status of darksend pool
//
class CDarkSendPool
{
public:
    static const int MIN_PEER_PROTO_VERSION = 70045;

    // clients entries
    std::vector<CDarkSendEntry> myEntries;
    // masternode entries
    std::vector<CDarkSendEntry> entries;
    // the finalized transaction ready for signing
    CTransaction finalTransaction;

    int64 lastTimeChanged;
    int64 lastAutoDenomination;

    unsigned int state; 
    unsigned int entriesCount;
    unsigned int lastEntryAccepted;
    unsigned int countEntriesAccepted;

    // where collateral should be made out to
    CScript collateralPubKey;

    std::vector<CTxIn> lockedCoins;
    
    uint256 masterNodeBlockHash;
     
    std::string lastMessage;
    bool completedTransaction;
    bool unitTest;
    CService submittedToMasternode;

    int sessionID;
    int sessionDenom; //Users must submit an denom matching this
    int sessionUsers; //N Users have said they'll join
    bool sessionFoundMasternode; //If we've found a compatible masternode
    int sessionTries;
    int64 sessionTotalValue; //used for autoDenom
    std::vector<CTransaction> vecSessionCollateral;

    int lastSplitUpBlock;
    int splitUpInARow; // how many splits we've done since a success?
    int cachedLastSuccess;
    int cachedNumBlocks; //used for the overview screen
    int minBlockSpacing; //required blocks between mixes
    CTransaction txCollateral;

    //incremented whenever a DSQ comes through
    int64 nDsqCount;

    CDarkSendPool()
    {
        /* DarkSend uses collateral addresses to trust parties entering the pool
            to behave themselves. If they don't it takes their money. */

        std::string strAddress = "";  
        if(!fTestNet) {
            strAddress = "Xq19GqFvajRrEdDHYRKGYjTsQfpV5jyipF";
        } else {
            strAddress = "mxE2Rp3oYpSEFdsN5TdHWhZvEHm3PJQQVm";
        }
        
        lastSplitUpBlock = 0;
        cachedLastSuccess = 0;
        cachedNumBlocks = 0;
        unitTest = false;
        splitUpInARow = 0;
        txCollateral = CTransaction();
        minBlockSpacing = 1;
        nDsqCount = 0;

        SetCollateralAddress(strAddress);
        SetNull();
    }

    void SetMinBlockSpacing(int minBlockSpacingIn){
        minBlockSpacing = minBlockSpacingIn;
    }

    bool SetCollateralAddress(std::string strAddress);
    void SetNull(bool clearEverything=false);

    void UnlockCoins();

    bool IsNull() const
    {   
        return (state == POOL_STATUS_ACCEPTING_ENTRIES && entries.empty() && myEntries.empty());
    }

    int GetState() const
    {
        return state;
    }

    int GetEntriesCount() const
    {
        if(fMasterNode){
            return entries.size(); 
        } else {
            return entriesCount;
        }
    }

    int GetLastEntryAccepted() const
    {
        return lastEntryAccepted;
    }

    int GetCountEntriesAccepted() const
    {
        return countEntriesAccepted;
    }

    int GetMyTransactionCount() const
    {
        return myEntries.size();
    }

    void UpdateState(unsigned int newState)
    {
        if (fMasterNode && (newState == POOL_STATUS_ERROR || newState == POOL_STATUS_SUCCESS)){
            LogPrintf("CDarkSendPool::UpdateState() - Can't set state to ERROR or SUCCESS as a masternode. \n");
            return;
        }

        LogPrintf("CDarkSendPool::UpdateState() == %d | %d \n", state, newState);
        if(state != newState){
            lastTimeChanged = GetTimeMillis();
            if(fMasterNode) {
                RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);
            }
        }
        state = newState;
    }

    int GetMaxPoolTransactions()
    {
        //if we're on testnet, just use two transactions per merge
        if(fTestNet) return 2;

        //use the production amount
        return POOL_MAX_TRANSACTIONS;
    }

    //Do we have enough users to take entries? 
    bool IsSessionReady(){
        return sessionUsers >= GetMaxPoolTransactions();
    }

    // Are these outputs compatible with other client in the pool?
    bool IsCompatibleWithEntries(std::vector<CTxOut> vout);
    // Is this amount compatible with other client in the pool?
    bool IsCompatibleWithSession(int64 nAmount, CTransaction txCollateral, std::string& strReason);

    // Passively run Darksend in the background according to the configuration in settings (only for QT)
    bool DoAutomaticDenominating(bool fDryRun=false, bool ready=false);


    // check for process in Darksend
    void Check();
    // charge fees to bad actors
    void ChargeFees();
    // rarely charge fees to pay miners
    void ChargeRandomFees();
    void CheckTimeout();
    // check to make sure a signature matches an input in the pool
    bool SignatureValid(const CScript& newSig, const CTxIn& newVin);
    // if the collateral is valid given by a client
    bool IsCollateralValid(const CTransaction& txCollateral);
    // add a clients entry to the pool
    bool AddEntry(const std::vector<CTxIn>& newInput, const int64& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error);
    // add signature to a vin
    bool AddScriptSig(const CTxIn& newVin);
    // are all inputs signed?
    bool SignaturesComplete();
    // as a client, send a transaction to a masternode to start the denomination process
    void SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64 amount);
    // get masternode updates about the progress of darksend
    bool StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID=0);

    // as a client, check and sign the final transaction
    bool SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node);

    // get block hash by height
    bool GetBlockHash(uint256& hash, int nBlockHeight);
    // get the last valid block hash for a given modulus
    bool GetLastValidBlockHash(uint256& hash, int mod=1, int nBlockHeight=0);
    // process a new block
    void NewBlock();
    void CompletedTransaction(bool error, std::string lastMessageNew);
    void ClearLastMessage();
    // used for liquidity providers
    bool SendRandomPaymentToSelf();
    // split up large inputs or make fee sized inputs
    bool SplitUpMoney(bool justCollateral=false);
    // get the denominations for a list of outputs (returns a bitshifted integer)
    int GetDenominations(const std::vector<CTxOut>& vout);
    // get the denominations for a specific amount of darkcoin. 
    int GetDenominationsByAmount(int64 nAmount);
};


void ConnectToDarkSendMasterNodeWinner();

void ThreadCheckDarkSendPool();

#endif