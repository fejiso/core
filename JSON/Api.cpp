
#include "Api.h"
#include "../Chain.h"
#include "../Tools/Hexdump.h"
#include "../CertStore/CertStore.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include "../Tools/Log.h"
#include "../BlockStore.h"
#include "../TxPool.h"
#include "../Wallet.h"
#include "../AddressStore.h"
#include "../AddressHelper.h"
#include "../BlockCreator/Mint.h"
#include "../Network/Network.h"
#include "../PassportReader/PKCS7/PKCS7Parser.h"
#include "../PassportReader/LDS/LDSParser.h"
#include "../PassportReader/Reader/Reader.h"
#include "../Network/Peers.h"
#include "../Transaction/TransactionHelper.h"
#include "../Consensus/VoteStore.h"
#include "../Time.h"
#include "../Network/NetworkCommands.h"
#include "../Network/BanList.h"
#include "../Crypto/CreateSignature.h"
#include "../Base64.h"

using boost::property_tree::ptree;

ptree error(std::string message) {
    ptree error;
    error.put("error", message);

    return error;
}

ptree uamountToPtree(UAmount uamount) {
    ptree uamountTree;

    for(std::map<uint8_t, CAmount>::iterator it = uamount.map.begin(); it != uamount.map.end(); it++) {
        if(it->second > 0) {
            uamountTree.put(std::to_string(it->first), std::to_string(it->second));
        }
    }

    return uamountTree;
}

ptree txInToPtree(TxIn txIn, bool checkIsMine) {
    ptree txInTree;

    txInTree.put("inAddress", Hexdump::vectorToHexString(txIn.getInAddress()));
    txInTree.put("scriptType", txIn.getScript().getScriptType());
    txInTree.put("script", Hexdump::vectorToHexString(txIn.getScript().getScript()));
    txInTree.push_back(std::make_pair("amount", uamountToPtree(txIn.getAmount())));
    txInTree.put("nonce", txIn.getNonce());

    if(checkIsMine) {
        Wallet& wallet = Wallet::Instance();
        txInTree.put("isMine", wallet.isMine(txIn.getInAddress()));
    }

    return txInTree;
}

ptree txOutToPtree(TxOut txOut, bool checkIsMine) {
    ptree txOutTree;

    txOutTree.put("scriptType", txOut.getScript().scriptType);
    txOutTree.put("script", Hexdump::vectorToHexString(txOut.getScript().getScript()));
    txOutTree.push_back(std::make_pair("amount", uamountToPtree(txOut.getAmount())));

    if(checkIsMine) {
        Wallet& wallet = Wallet::Instance();
        txOutTree.put("isMine", wallet.isMine(txOut.getScript()));
    }

    return txOutTree;
}

ptree statusListToPtree(std::vector<std::pair<uint32_t, bool> > statusList) {
    ptree statusListTree;

    for(auto status : statusList) {
        ptree statusTree;
        statusTree.put("blockHeight", status.first);
        statusTree.put("active", status.second);
        statusListTree.push_back(std::make_pair("", statusTree));
    }


    return statusListTree;
}

ptree txToPtree(Transaction transaction, bool checkIsMine) {
    ptree txInTree;
    ptree txOutTree;
    ptree transactionTree;
    transactionTree.put("txId", Hexdump::vectorToHexString(TransactionHelper::getTxId(&transaction)));
    transactionTree.put("network", transaction.getNetwork());

    UAmount inAmount;
    UAmount outAmount;

    for (auto txIn: transaction.getTxIns()) {
        txInTree.push_back(std::make_pair("", txInToPtree(txIn, checkIsMine)));
        inAmount += txIn.getAmount();
    }

    for (auto txOut: transaction.getTxOuts()) {
        txOutTree.push_back(std::make_pair("", txOutToPtree(txOut, checkIsMine)));
        outAmount += txOut.getAmount();
    }

    transactionTree.push_back(std::make_pair("fee", uamountToPtree(inAmount - outAmount)));
    transactionTree.push_back(std::make_pair("txIn", txInTree));
    transactionTree.push_back(std::make_pair("txOut", txOutTree));

    return transactionTree;
}

ptree uamountToPtree(UAmount32 uamount) {
    ptree uamountTree;

    for(std::map<uint8_t, CAmount32>::iterator it = uamount.map.begin(); it != uamount.map.end(); it++) {
        uamountTree.put(std::to_string(it->first), std::to_string(it->second));
    }

    return uamountTree;
}

std::string Api::vote(std::string json) {
    std::vector<unsigned char> targetPubKey;
    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    Peers &peers = Peers::Instance();
    bool success = true;

    bool removedPeer = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "targetPubKey") == 0) {
            targetPubKey = Hexdump::hexStringToVector(v.second.data());
        }
    }

    if(targetPubKey.empty()) {
        return "{\"success\": false, \"error\": \"missing targetPubKey field\"}";
    }

    TxPool& txPool = TxPool::Instance();
    Wallet& wallet = Wallet::Instance();
    VoteStore& voteStore = VoteStore::Instance();
    std::map<std::vector<unsigned char>, Delegate> activeDelegates = voteStore.getActiveDelegates();

    // get through all active delegates and verify if it is me
    for (auto activeDelegate: activeDelegates) {
        Address address = wallet.addressFromPublicKey(activeDelegate.first);
        if(wallet.isMine(address.getScript())) {
            Transaction* transaction = new Transaction();
            Vote* vote = new Vote();

            vote->setAction(VOTE_ACTION_VOTE);
            vote->setTargetPubKey(targetPubKey);

            CDataStream s(SER_DISK, 1);
            s << *vote;
            UScript outScript;
            outScript.setScript(std::vector<unsigned char>(s.data(), s.data() + s.size()));
            outScript.setScriptType(SCRIPT_VOTE);
            TxOut* txOut = new TxOut();
            txOut->setScript(outScript);

            std::vector<TxOut> txOuts;
            txOuts.push_back(*txOut);

            transaction->setTxOuts(txOuts);

            transaction->setNetwork(NET_CURRENT);

            UScript inScript;
            inScript.setScriptType(SCRIPT_VOTE);

            TxIn *txIn = new TxIn();
            txIn->setNonce(activeDelegate.second.getNonce());
            txIn->setScript(inScript);
            txIn->setInAddress(activeDelegate.second.getPublicKey());

            std::vector<TxIn> txIns;
            txIns.push_back(*txIn);

            transaction->setTxIns(txIns);

            std::vector<unsigned char> signature = wallet.signWithAddress(
                    AddressHelper::addressLinkFromScript(address.getScript()),
                    TransactionHelper::getTxId(transaction)
            );

            inScript.setScript(signature);
            TxIn txInWithSignature = transaction->getTxIns().front();
            txInWithSignature.setScript(inScript);

            std::vector<TxIn> txInsWithSignature;
            txInsWithSignature.push_back(txInWithSignature);

            transaction->setTxIns(txInsWithSignature);

            Chain& chain = Chain::Instance();
            if(TransactionHelper::verifyTx(transaction, IS_IN_HEADER, chain.getBestBlockHeader())) {
                txPool.appendTransaction(*transaction);
            } else {
                success = false;
            }
        }
    }

    if(success) {
        return "{\"success\": true}";
    } else {
        return "{\"success\": false, \"error\": \"Failed to verify transaction\"}";
    }

}

std::string Api::unvote(std::string json) {

    std::vector<unsigned char> targetPubKey;
    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    Peers &peers = Peers::Instance();
    bool success = true;

    bool removedPeer = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "targetPubKey") == 0) {
            targetPubKey = Hexdump::hexStringToVector(v.second.data());
        }
    }

    if(targetPubKey.empty()) {
        return "{\"success\": false, \"error\": \"missing targetPubKey field\"}";
    }

    TxPool& txPool = TxPool::Instance();
    Wallet& wallet = Wallet::Instance();
    VoteStore& voteStore = VoteStore::Instance();
    std::map<std::vector<unsigned char>, Delegate> activeDelegates = voteStore.getActiveDelegates();

    // get through all active delegates and verify if it is me
    for (auto activeDelegate: activeDelegates) {
        Address address = wallet.addressFromPublicKey(activeDelegate.first);
        if(wallet.isMine(address.getScript())) {
            Transaction* transaction = new Transaction();
            Vote* vote = new Vote();

            vote->setAction(VOTE_ACTION_UNVOTE);
            vote->setTargetPubKey(targetPubKey);

            CDataStream s(SER_DISK, 1);
            s << *vote;
            UScript outScript;
            outScript.setScript(std::vector<unsigned char>(s.data(), s.data() + s.size()));
            outScript.setScriptType(SCRIPT_VOTE);
            TxOut* txOut = new TxOut();
            txOut->setScript(outScript);

            std::vector<TxOut> txOuts;
            txOuts.push_back(*txOut);

            transaction->setTxOuts(txOuts);

            transaction->setNetwork(NET_CURRENT);

            UScript inScript;
            inScript.setScriptType(SCRIPT_VOTE);

            TxIn *txIn = new TxIn();
            txIn->setNonce(activeDelegate.second.getNonce());
            txIn->setScript(inScript);
            txIn->setInAddress(activeDelegate.second.getPublicKey());

            std::vector<TxIn> txIns;
            txIns.push_back(*txIn);

            transaction->setTxIns(txIns);

            std::vector<unsigned char> signature = wallet.signWithAddress(
                    AddressHelper::addressLinkFromScript(address.getScript()),
                    TransactionHelper::getTxId(transaction)
            );

            inScript.setScript(signature);
            TxIn txInWithSignature = transaction->getTxIns().front();
            txInWithSignature.setScript(inScript);

            std::vector<TxIn> txInsWithSignature;
            txInsWithSignature.push_back(txInWithSignature);

            transaction->setTxIns(txInsWithSignature);

            Chain& chain = Chain::Instance();
            if(TransactionHelper::verifyTx(transaction, IS_IN_HEADER, chain.getBestBlockHeader())) {
                txPool.appendTransaction(*transaction);
            } else {
                success = false;
            }
        }
    }

    if(success) {
        return "{\"success\": true}";
    } else {
        return "{\"success\": false, \"error\": \"Failed to verify transaction\"}";
    }

}

std::string Api::getDelegates() {
    VoteStore& voteStore = VoteStore::Instance();
    Wallet& wallet = Wallet::Instance();
    std::map<std::vector<unsigned char>, Delegate> activeDelegates = voteStore.getActiveDelegates();
    std::map<std::vector<unsigned char>, Delegate> allDelegates = voteStore.getAllDelegates();

    std::vector<unsigned char> currentDelegateId = voteStore.getValidatorForTimestamp(Time::getCurrentTimestamp());
    ptree baseTree;
    ptree delegatesTree;
    for(auto delegate: allDelegates) {
        bool isActive = false;

        if(activeDelegates.find(delegate.first) != activeDelegates.end()) {
            isActive = true;
        }
        ptree delegateTree;
        ptree votesTree;

        delegateTree.put("pubKey", Hexdump::vectorToHexString(delegate.second.getPublicKey()));
        delegateTree.put("nonce", delegate.second.getNonce());
        delegateTree.put("totalVote", delegate.second.getTotalVote());
        delegateTree.put("votes", delegate.second.getVoteCount());
        delegateTree.put("unVotes", delegate.second.getUnVoteCount());
        delegateTree.put("lastVotedInBlock", Hexdump::vectorToHexString(delegate.second.getBlockHashLastVote()));
        delegateTree.put("isActive", isActive);
        delegateTree.put("isCurrent", currentDelegateId == delegate.first);
        delegateTree.put("isMe", wallet.isMine(
                AddressHelper::addressLinkFromScript(
                        wallet.addressFromPublicKey(delegate.second.getPublicKey()).getScript()
                )
        ));

        std::vector<Vote> votes = delegate.second.getVotes();
        for (auto vote: votes) {
            ptree voteTree;
            voteTree.put("nonce", vote.getNonce());
            voteTree.put("targetPubKey", Hexdump::vectorToHexString(vote.getTargetPubKey()));
            voteTree.put("action", vote.getAction());
            voteTree.put("fromPubKey", Hexdump::vectorToHexString(vote.getFromPubKey()));
            voteTree.put("version", vote.getVersion());
            votesTree.push_back(std::make_pair("", voteTree));
        }
        delegateTree.push_back(std::make_pair("votes", votesTree));

        delegatesTree.push_back(std::make_pair("", delegateTree));
    }

    baseTree.add_child("delegates", delegatesTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::removePeer(std::string json) {
    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    Peers &peers = Peers::Instance();

    bool removedPeer = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "ip") == 0) {
            peers.disconnect(v.second.data());
            removedPeer = true;
        }
    }

    if(removedPeer) {
        return "{\"success\": true}";
    }

    return "{\"success\": false}";
}

std::string Api::addPeer(std::string json) {
    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    Peers &peers = Peers::Instance();

    bool addPeer = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "ip") == 0) {
            auto io_service = make_shared<boost::asio::io_service>();
            tcp::resolver resolver(*io_service);

            auto work = make_shared<boost::asio::io_service::work>(*io_service);

            //@TODO check it is a ip v4 using regex
            std::string ip;
            ip = v.second.data();
            Log(LOG_LEVEL_INFO) << "ip: " << ip;
            auto endpoint_iterator = resolver.resolve({ip, NET_PORT});

            auto peer = make_shared<PeerClient>(io_service, endpoint_iterator, work);

            //std::shared_ptr<PeerClient> nPeer = peer->get();
            peer->setBlockHeight(0);
            peer->setIp(ip);
            if(peers.appendPeer(peer->get())) {
                peer->do_connect();

                auto io_service_run = [io_service]() {
                    try{
                        io_service->run();
                        //io_service->stop();
                        Log(LOG_LEVEL_INFO) << "io_service terminated";
                    }
                    catch (const std::exception& e) {
                        Log(LOG_LEVEL_ERROR) << "io_service.run terminated with: " << e.what();
                    }
                };
                std::thread t(io_service_run);
                t.detach();

                Chain& chain = Chain::Instance();

                // transmit our own block height
                TransmitBlockchainHeight *transmitBlockchainHeight = new TransmitBlockchainHeight();
                transmitBlockchainHeight->height = chain.getCurrentBlockchainHeight();
                peer->deliver(NetworkMessageHelper::serializeToNetworkMessage(*transmitBlockchainHeight));

                //ask for blockheight
                AskForBlockchainHeight askForBlockchainHeight;
                peer->deliver(NetworkMessageHelper::serializeToNetworkMessage(askForBlockchainHeight));

                //ask for donation Address
                AskForDonationAddress askForDonationAddress;
                peer->deliver(NetworkMessageHelper::serializeToNetworkMessage(askForDonationAddress));

                addPeer = true;
            } else {
                peer->close();
            }
        }
    }

    if(addPeer) {
        return "{\"success\": true}";
    }

    return "{\"success\": false}";
}

std::string Api::getPeers() {
    Peers &peers = Peers::Instance();

    std::map<std::string, PeerInterfacePtr> peerList = peers.getPeers();
    ptree baseTree;
    ptree peersTree;

    for(auto peer: peerList) {
        ptree peerTree;

        peerTree.put("ip", peer.second->getIp());
        peerTree.put("blockHeight", peer.second->getBlockHeight());
        peerTree.put("donationAddress", peer.second->getDonationAddress());

        peersTree.push_back(std::make_pair("", peerTree));
    }

    baseTree.add_child("peers", peersTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getAddress(std::vector<unsigned char> address) {
    AddressStore &addressStore = AddressStore::Instance();
    AddressForStore addressForStore = addressStore.getAddressFromStore(address);

    if(addressForStore.getScript().getScript().empty()) {
        return "{\"success\": false, \"error\": \"address not found\"}";
    }

    ptree baseTree;
    ptree addressTree;

    addressTree.put("nonce", addressForStore.getNonce());
    addressTree.put("scriptType", addressForStore.getScript().getScriptType());
    addressTree.put("script", Hexdump::vectorToHexString(addressForStore.getScript().getScript()));
    addressTree.push_back(std::make_pair("amountWithUBI", uamountToPtree(AddressHelper::getAmountWithUBI(&addressForStore))));
    addressTree.push_back(std::make_pair("amountWithoutUBI", uamountToPtree(addressForStore.getAmount())));
    addressTree.push_back(std::make_pair("UBIdebit", uamountToPtree(addressForStore.getUBIdebit())));
    addressTree.put("DscCertificate", Hexdump::vectorToHexString(addressForStore.getDscCertificate()));
    addressTree.put("DSCLinkedAtHeight", addressForStore.getDSCLinkedAtHeight());

    baseTree.add_child("address", addressTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::removeBan(std::string json) {
    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    BanList& banList = BanList::Instance();

    bool removedBan = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "ip") == 0) {
            banList.removeFromBanList(v.second.data());
            removedBan = true;
        }
    }

    if(removedBan) {
        return "{\"success\": true}";
    }

    return "{\"success\": false}";
}

std::string Api::addBan(std::string json) {
    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    BanList& banList = BanList::Instance();

    bool addedBan = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "ip") == 0) {
            banList.appendBan(v.second.data(), BAN_INC_INSTA_BAN);
            addedBan = true;
        }
    }

    if(addedBan) {
        return "{\"success\": true}";
    }

    return "{\"success\": false}";
}

std::string Api::getBans() {
    BanList& banList = BanList::Instance();
    std::map<ip_t, uint16_t> bans = banList.getBanList();

    ptree baseTree;
    ptree bansTree;

    for(auto ban: bans) {
        ptree peerTree;

        peerTree.put("ip", ban.first);
        peerTree.put("score", ban.second);

        bansTree.push_back(std::make_pair("", peerTree));
    }

    baseTree.add_child("bans", bansTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::myTransactions() {
    Wallet &wallet = Wallet::Instance();
    Chain& chain = Chain::Instance();

    ptree baseTree;
    ptree transactionsTree;
    uint32_t txNbr = 0;
    auto myTransactions = wallet.getMyTransactions();
    for (std::vector<TransactionForStore>::reverse_iterator it = myTransactions.rbegin(); it != myTransactions.rend();it++) {
        ptree txInTree;
        ptree txOutTree;

        BlockHeader* header = chain.getBlockHeader(it->getBlockHash());
        if(header != nullptr) {
            transactionsTree.push_back(std::make_pair("", txToPtree(it->getTx(), true)));
            txNbr++;
            if(txNbr > MAX_NUMBER_OF_MY_TRANSACTIONS_TO_DISPLAY) {
                break;
            }
        }
    }

    baseTree.add_child("transactions", transactionsTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::readPassport(std::string json) {

    Wallet &wallet = Wallet::Instance();

    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);

    std::string documentNumber;
    std::string dateOfBirth;
    std::string dateOfExpiry;

    for (boost::property_tree::ptree::value_type &v : pt)
    {
        if(strcmp(v.first.data(), "documentNumber") == 0) {
            documentNumber = v.second.data();
            if(documentNumber.size() != 9) {
                return "{\"success\": false, \"error\" : \"documentNumber should be of length 9\"}";
            }
        }
        if(strcmp(v.first.data(), "dateOfBirth") == 0) {
            dateOfBirth = v.second.data();
            if(dateOfBirth.size() != 6) {
                return "{\"success\": false, \"error\" : \"dateOfBirth should be of length 6\"}";
            }
        }
        if(strcmp(v.first.data(), "dateOfExpiry") == 0) {
            dateOfExpiry = v.second.data();
            if(dateOfExpiry.size() != 6) {
                return "{\"success\": false, \"error\" : \"dateOfExpiry should be of length 6\"}";
            }
        }
    }

    BacKeys* bacKeys = new BacKeys();
    bacKeys->setDateOfBirth(dateOfBirth);
    bacKeys->setDateOfExpiry(dateOfExpiry);
    bacKeys->setDocumentNumber(documentNumber);

    SessionKeys *sessionKeys = new SessionKeys;

    Reader* reader = new Reader();
    if(reader->initConnection(bacKeys, sessionKeys))
    {
        unsigned char file[64000];
        unsigned int fileSize;
        unsigned char fileId[3] = {'\x01', '\x1D', '\0'}; // SOD

        if(!reader->readFile(fileId, file, &fileSize, sessionKeys)) {
            reader->close();
            return "{\"success\": false, \"error\" : \"failed to read SOD file\"}";
        }
        reader->close();

        Hexdump::dump(file, fileSize);

        LDSParser* ldsParser = new LDSParser(file, fileSize);

        unsigned char sod[32000];
        unsigned int sodSize = 0;

        ldsParser->getTag((unsigned char*)"\x77")
                ->getContent(sod, &sodSize);

        Hexdump::dump(sod, sodSize);

        PKCS7Parser* pkcs7Parser = new PKCS7Parser((char*)sod, sodSize);

        if(pkcs7Parser->hasError()) {
            Log() << "Pkcs7Parser has an error";
            return "{\"success\": false, \"error\" : \"Pkcs7Parser has an error\"}";
        }


        Cert* pkcsCert = new Cert();
        Address randomWalletAddress = wallet.getRandomAddressFromWallet();
        pkcsCert->setX509(pkcs7Parser->getDscCertificate());

        Transaction* registerPassportTx = new Transaction();
        TxIn* pTxIn = new TxIn();
        UScript* pIScript = new UScript();
        pIScript->setScript(std::vector<unsigned char>());
        pIScript->setScriptType(SCRIPT_REGISTER_PASSPORT);
        pTxIn->setInAddress(pkcsCert->getId());
        pTxIn->setScript(*pIScript);
        pTxIn->setNonce(0);
        pTxIn->setAmount(*(new UAmount()));
        registerPassportTx->addTxIn(*pTxIn);

        TxOut* pTxOut = new TxOut();
        pTxOut->setAmount(*(new UAmount()));
        pTxOut->setScript(randomWalletAddress.getScript());
        registerPassportTx->addTxOut(*pTxOut);
        registerPassportTx->setNetwork(NET_CURRENT);

        Log(LOG_LEVEL_INFO) << "randomWalletAddressScript : " << AddressHelper::addressLinkFromScript(randomWalletAddress.getScript());

        std::vector<unsigned char> txId = TransactionHelper::getTxId(registerPassportTx);

        if(pkcs7Parser->isRSA()) {
            NtpRskSignatureRequestObject *ntpRskSignatureRequestObject = pkcs7Parser->getNtpRsk();

            // @TODO perhaps add padding to txId
            ntpRskSignatureRequestObject->setNm(ECCtools::vectorToBn(txId));

            NtpRskSignatureVerificationObject *ntpEskSignatureVerificationObject = NtpRsk::signWithNtpRsk(
                    ntpRskSignatureRequestObject
            );

            CDataStream sntpRsk(SER_DISK, 1);
            sntpRsk << *ntpEskSignatureVerificationObject;

            Log(LOG_LEVEL_INFO) << "generated NtpRsk: " << sntpRsk;
            pIScript->setScript((unsigned char *) sntpRsk.data(), (uint16_t) sntpRsk.size());

            pTxIn->setScript(*pIScript);

        } else {

            NtpEskSignatureRequestObject *ntpEskSignatureRequestObject = pkcs7Parser->getNtpEsk();
            ntpEskSignatureRequestObject->setNewMessageHash(txId);

            Log(LOG_LEVEL_INFO) << "P-UID, Passport unique identifier (signed hash):: "
                                << ntpEskSignatureRequestObject->getMessageHash();

            std::string dscId = pkcsCert->getIdAsHexString();
            Log(LOG_LEVEL_INFO) << "dscId: " << dscId;
            Log(LOG_LEVEL_INFO) << "subject: "
                                << X509_NAME_oneline(X509_get_subject_name(pkcs7Parser->getDscCertificate()), 0, 0);


            NtpEskSignatureVerificationObject *ntpEskSignatureVerificationObject = NtpEsk::signWithNtpEsk(
                    ntpEskSignatureRequestObject);

            CDataStream sntpEsk(SER_DISK, 1);
            sntpEsk << *ntpEskSignatureVerificationObject;
            Log(LOG_LEVEL_INFO) << "generated NtpEsk: " << sntpEsk;
            pIScript->setScript((unsigned char *) sntpEsk.data(), (uint16_t) sntpEsk.size());

            pTxIn->setScript(*pIScript);
        }
        std::vector<TxIn> pTxIns;
        pTxIns.push_back(*pTxIn);
        registerPassportTx->setTxIns(pTxIns);

        Chain& chain = Chain::Instance();
        if(TransactionHelper::verifyTx(registerPassportTx, IS_NOT_IN_HEADER, chain.getBestBlockHeader())) {
            Log(LOG_LEVEL_INFO) << "Passport transaction verified";
        } else {

            char pData[512];
            FS::charPathFromVectorPath(pData, FS::concatPaths(FS::getConfigBasePath(), "extractedDSC.cert"));
            FILE* fileDSC = fopen (pData , "w");

            if(fileDSC != nullptr) {
                Log(LOG_LEVEL_INFO) << "wrote to extractedDSC.cert file";
                i2d_X509_fp(fileDSC, pkcsCert->getX509());
                fclose(fileDSC);
            }

           return "{\"success\": false, \"error\" : \"couldn't verify Passport transaction\"}";
        }

        CDataStream spTx(SER_DISK, 1);
        spTx << *registerPassportTx;

        Log(LOG_LEVEL_INFO) << spTx;

        printf("register passport tx: ");
        Hexdump::dump((unsigned char*)spTx.data(), (uint16_t)spTx.size());

        TxPool& txPool = TxPool::Instance();
        if(txPool.appendTransaction(*registerPassportTx)) {
            Network &network = Network::Instance();
            network.broadCastTransaction(*registerPassportTx);
            return "{\"success\": true}";
        } else {
            return "{\"success\": false, \"error\" : \"Cannot append transaction to txPool, may be this passport is already registered\"}";
        }

    } else {
        return "{\"success\": false, \"error\" : \"couldn't read NFC chip\"}";
    }

    return "{\"success\": false}";

}

std::string Api::pay(std::string json) {

    Wallet &wallet = Wallet::Instance();

    if (json.empty()) {
        return "{\"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);

    std::vector<TxOut> txOuts;

    for (boost::property_tree::ptree::value_type &v : pt) {
        TxOut txOut;
        if(!wallet.verifyReadableAddressChecksum(v.first.data())) {
            return "{\"error\": \"invalid address\"}";
        }
        std::vector<unsigned char> vectorAddress = wallet.readableAddressToVectorAddress(v.first.data());
        Address address;
        CDataStream s(SER_DISK, 1);
        s.write((char *) vectorAddress.data(), vectorAddress.size());
        s >> address;

        txOut.setScript(address.getScript());

        std::cout << v.first.data() << std::endl;
        UAmount uAmountAggregated;
        for (boost::property_tree::ptree::value_type &v2 : v.second) {
            std::cout << v2.first.data() << std::endl;
            std::cout << v2.second.get_value<uint64_t>() << std::endl;

            UAmount uAmount;
            uAmount.map.insert(std::make_pair((uint8_t) atoi(v2.first.data()), v2.second.get_value<uint64_t>()));
            uAmountAggregated += uAmount;
        }
        txOut.setAmount(uAmountAggregated);
        txOuts.push_back(txOut);
    }

    Transaction *tx = wallet.payToTxOutputs(txOuts);

    if (tx == nullptr) {
        return "{\"success\": false}";
    }

    TxPool &txPool = TxPool::Instance();
    if (txPool.appendTransaction(*tx)) {

        Network &network = Network::Instance();
        network.broadCastTransaction(*tx);

        return "{\"success\": true}";
    }

    return "{\"success\": false}";
}

std::string Api::createTransaction(std::string json) {
    Wallet &wallet = Wallet::Instance();

    if (json.empty()) {
        return "{\"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);

    std::vector<TxOut> txOuts;

    for (boost::property_tree::ptree::value_type &v : pt) {
        TxOut txOut;
        if(!wallet.verifyReadableAddressChecksum(v.first.data())) {
            return "{\"error\": \"invalid address\"}";
        }
        std::vector<unsigned char> vectorAddress = wallet.readableAddressToVectorAddress(v.first.data());
        Address address;
        CDataStream s(SER_DISK, 1);
        s.write((char *) vectorAddress.data(), vectorAddress.size());
        s >> address;

        txOut.setScript(address.getScript());

        std::cout << v.first.data() << std::endl;
        UAmount uAmountAggregated;
        for (boost::property_tree::ptree::value_type &v2 : v.second) {
            std::cout << v2.first.data() << std::endl;
            std::cout << v2.second.get_value<uint64_t>() << std::endl;

            UAmount uAmount;
            uAmount.map.insert(std::make_pair((uint8_t) atoi(v2.first.data()), v2.second.get_value<uint64_t>()));
            uAmountAggregated += uAmount;
        }
        txOut.setAmount(uAmountAggregated);
        txOuts.push_back(txOut);
    }

    Transaction *tx = wallet.payToTxOutputs(txOuts);

    CDataStream s2(SER_DISK, 1);
    if (tx == nullptr) {
        return "{\"success\": false}";
    }

    s2 << *tx;
    std::string tx64 = base64_encode((unsigned char*)s2.str().data(), (uint32_t)s2.str().size());
    ptree baseTree;

    baseTree.put("success", true);
    baseTree.push_back(std::make_pair("transaction", txToPtree(*tx, false)));
    baseTree.put("base64", tx64);

    std::stringstream ss2;
    boost::property_tree::json_parser::write_json(ss2, baseTree);

    return ss2.str();
}

std::string Api::sendTransaction(std::string json) {
    Wallet &wallet = Wallet::Instance();

    if (json.empty()) {
        return "{\"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);

    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "base64") == 0) {
            CDataStream s(SER_DISK, 1);
            std::string txString = base64_decode(v.second.data());
            s.write(txString.c_str(), txString.length());

            Transaction tx;
            try {
                s >> tx;
            } catch (const std::exception& e) {
                Log(LOG_LEVEL_ERROR) << "Cannot deserialize base64 encoded transaction";
                return "{\"success\": false}";
            }

            TxPool &txPool = TxPool::Instance();
            if (txPool.appendTransaction(tx)) {

                Network &network = Network::Instance();
                network.broadCastTransaction(tx);

                return "{\"success\": true}";
            } else {
                return "{\"success\": false}";
            }
        }
    }
    return "{\"error\": \"missing base64 parameter\"}";
}

void startMintingThread() {
    Mint& mint = Mint::Instance();
    mint.startMinting();
}

std::string Api::startMint() {
    std::thread t1(&startMintingThread);
    t1.detach();
    return "{\"done\": true}";
}

std::string Api::stopMint() {
    Mint& mint = Mint::Instance();
    mint.stopMinting();
    return "{\"done\": true}";
}

std::string Api::mintStatus() {
    Mint& mint = Mint::Instance();
    if(mint.getStatus()) {
        return "{\"minting\": true}";
    }
    return "{\"minting\": false}";
}

std::string Api::getUbi() {
    Wallet &wallet = Wallet::Instance();

    ptree baseTree;
    ptree ubisTree;
    for(std::vector<unsigned char> addressScript: wallet.getAddressesScript()) {
        ptree ubiTree;
        UScript addressUScript;
        addressUScript.setScript(addressScript);
        addressUScript.setScriptType(SCRIPT_PKH);

        std::vector<unsigned char> addressLink = AddressHelper::addressLinkFromScript(addressUScript);

        AddressStore &addressStore = AddressStore::Instance();
        AddressForStore addressForStore = addressStore.getAddressFromStore(addressLink);

        if(UBICalculator::isAddressConnectedToADSC(&addressForStore)) {
            UAmount received = UBICalculator::totalReceivedUBI(&addressForStore);
            ubiTree.put("startedAtBlockHeight", addressForStore.getDSCLinkedAtHeight());
            ubiTree.put("dscCertificateId", Hexdump::vectorToHexString(addressForStore.getDscCertificate()));
            ubiTree.push_back(std::make_pair("totalUbiReceived", uamountToPtree(received)));

            ubisTree.push_back(std::make_pair("", ubiTree));
        }
    }

    baseTree.push_back(std::make_pair("ubis", ubisTree));
    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getWallet() {
    Wallet &wallet = Wallet::Instance();

    ptree baseTree;
    ptree addressesTree;
    UAmount balance;
    for(std::vector<unsigned char> addressScript: wallet.getAddressesScript()) {
        ptree addressTree;
        UScript addressUScript;
        addressUScript.setScript(addressScript);
        addressUScript.setScriptType(SCRIPT_PKH);

        std::vector<unsigned char> addressLink = AddressHelper::addressLinkFromScript(addressUScript);

        //Log(LOG_LEVEL_INFO) << "Wallet lookup for " << addressLink;
        AddressStore &addressStore = AddressStore::Instance();
        AddressForStore addressForStore = addressStore.getAddressFromStore(addressLink);

        UAmount addressBalance = AddressHelper::getAmountWithUBI(&addressForStore);
        balance += addressBalance;

        Address* address = new Address();
        address->setScript(addressUScript);

        addressTree.put("readable", Wallet::readableAddressFromAddress(*address));
        addressTree.put("addressLink", Hexdump::vectorToHexString(addressLink));
        addressTree.put("hexscript", Hexdump::vectorToHexString(addressScript));
        addressTree.put("pubKey", Hexdump::vectorToHexString(wallet.getPublicKeyFromAddressLink(addressLink)));
        addressTree.push_back(std::make_pair("amount", uamountToPtree(addressBalance)));
        addressesTree.push_back(std::make_pair("", addressTree));

        if(addressBalance > 0) {
            Log(LOG_LEVEL_INFO) << "addressBalance: " << addressBalance;
        }
    }

    baseTree.push_back(std::make_pair("addresses", addressesTree));
    baseTree.push_back(std::make_pair("total", uamountToPtree(balance)));

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getTxPool() {
    TxPool &txPool = TxPool::Instance();

    ptree baseTree;
    ptree transactionsTree;
    for(auto transaction: txPool.getTransactionList()) {
        transactionsTree.push_back(std::make_pair("", txToPtree(transaction.second, true)));
    }

    baseTree.add_child("transactions", transactionsTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getIncomingTx() {
    TxPool &txPool = TxPool::Instance();
    Wallet& wallet = Wallet::Instance();

    ptree baseTree;
    ptree transactionsTree;
    for(auto transaction: txPool.getTransactionList()) {
        for(auto txOut : transaction.second.getTxOuts()) {
            if(wallet.isMine(txOut.getScript())) {
                transactionsTree.push_back(std::make_pair("", txToPtree(transaction.second, true)));
                break;
            }
        }
    }

    baseTree.add_child("transactions", transactionsTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getBlock(uint32_t blockHeight) {
    Chain& chain = Chain::Instance();
    BlockHeader* blockHeader = chain.getBlockHeader(blockHeight);

    if(blockHeader != nullptr) {
        return Api::getBlock(blockHeader->getHeaderHash());
    }

    return "{\"error\": \"Block not found\"}";
}

std::string Api::getBlock(std::vector<unsigned char> blockHeaderHash) {

    Chain& chain = Chain::Instance();
    BlockHeader* blockHeader = chain.getBlockHeader(blockHeaderHash);

    if(blockHeader == NULL) {
        Log(LOG_LEVEL_WARNING) << "BlockHeader with hash " << blockHeaderHash << "was not found";

        std::stringstream ss;
        boost::property_tree::json_parser::write_json(ss, error("Block not found!"));

        return ss.str();
    }

    ptree baseTree;
    ptree blockHeaderTree;
    ptree transactionsTree;

    blockHeaderTree.put("headerHash", Hexdump::vectorToHexString(blockHeaderHash));
    blockHeaderTree.put("previousHeaderHash", Hexdump::vectorToHexString(blockHeader->getPreviousHeaderHash()));
    blockHeaderTree.put("merkleRootHash", Hexdump::vectorToHexString(blockHeader->getMerkleRootHash()));
    blockHeaderTree.put("blockHeight", blockHeader->getBlockHeight());
    blockHeaderTree.put("timestamp", blockHeader->getTimestamp());
    blockHeaderTree.put("issuerPubKey", Hexdump::vectorToHexString(blockHeader->getIssuerPubKey()));
    blockHeaderTree.put("issuerSignature", Hexdump::vectorToHexString(blockHeader->getIssuerSignature()));
    blockHeaderTree.push_back(std::make_pair("payout", uamountToPtree(blockHeader->getPayout())));
    blockHeaderTree.push_back(std::make_pair("payoutRemainder", uamountToPtree(blockHeader->getPayoutRemainder())));
    blockHeaderTree.push_back(std::make_pair("ubiReceiverCount", uamountToPtree(blockHeader->getUbiReceiverCount())));
    ptree votesTree;
    for(auto vote: blockHeader->getVotes()) {
        votesTree.push_back(std::make_pair("", txToPtree(vote, false)));
    }
    blockHeaderTree.push_back(std::make_pair("votes", votesTree));

    baseTree.add_child("blockHeader", blockHeaderTree);

    Block* block = BlockStore::getBlock(blockHeaderHash);

    std::vector<Transaction> transactionList = block->getTransactions();

    for(auto transaction: transactionList) {
        transactionsTree.push_back(std::make_pair("", txToPtree(transaction, false)));
    }

    baseTree.add_child("transactions", transactionsTree);

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getIndex() {
    Chain& chain = Chain::Instance();
    Network& network = Network::Instance();
    Peers& peers = Peers::Instance();

    ptree baseTree;
    ptree bestBlock;

    BlockHeader* header = chain.getBestBlockHeader();
    if(header == nullptr) {
        bestBlock.put("hash", "(none)");
    } else {
        bestBlock.put("hash", Hexdump::vectorToHexString(chain.getBestBlockHeader()->getHeaderHash()));
    }
    bestBlock.put("height", chain.getCurrentBlockchainHeight());

    baseTree.add_child("bestBlock", bestBlock);
    baseTree.put("synced", network.synced);
    baseTree.put("peersCount", peers.getPeers().size());

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getRootCertificates() {
    CertStore& certStore = CertStore::Instance();

    ptree baseTree;

    std::map<std::vector<unsigned char>, Cert> rootList = certStore.getRootList();

    for(std::map<std::vector<unsigned char>, Cert>::iterator it = rootList.begin(); it != rootList.end(); it++) {
        ptree cert;
        cert.put("active", it->second.isCertAtive());
        cert.put("currency", it->second.getCurrencyId());
        cert.put("expirationDate", it->second.getExpirationDate());
        baseTree.add_child(Hexdump::vectorToHexString(it->first), cert);
    }

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getCSCACertificates() {
    CertStore& certStore = CertStore::Instance();

    ptree baseTree;

    std::map<std::vector<unsigned char>, Cert>* cscaList = certStore.getCSCAList();

    for(std::map<std::vector<unsigned char>, Cert>::iterator it = cscaList->begin(); it != cscaList->end(); it++) {
        ptree cert;
        cert.put("active", it->second.isCertAtive());
        cert.put("currency", it->second.getCurrencyId());
        cert.put("expirationDate", it->second.getExpirationDate());
        cert.put("rootSignature", Hexdump::vectorToHexString(it->second.getRootSignature()));
        baseTree.add_child(Hexdump::vectorToHexString(it->first), cert);
    }

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}

std::string Api::getDSCCertificate(std::string dscIdString) {
    CertStore& certStore = CertStore::Instance();

    Cert* dsc = certStore.getDscCertWithCertId(Hexdump::hexStringToVector(dscIdString));
    if(dsc == nullptr) {
        return "{\"error\": \"DSC not found\"}";
    }

    ptree cert;
    cert.push_back(std::make_pair("statusList", statusListToPtree(dsc->getStatusList())));
    cert.put("active", dsc->isCertAtive());
    cert.put("currency", dsc->getCurrencyId());
    cert.put("expirationDate", dsc->getExpirationDate());
    cert.put("rootSignature", Hexdump::vectorToHexString(dsc->getRootSignature()));

    BIO *mem = BIO_new(BIO_s_mem());
    X509_print(mem, dsc->getX509());
    char* x509Buffer;
    BIO_get_mem_data(mem, &x509Buffer);

    BIO_set_close(mem, BIO_CLOSE);
    BIO_free(mem);

    cert.put("x509", (std::string)(x509Buffer));

    X509_NAME *name = X509_get_issuer_name(dsc->getX509());

    char* charName = X509_NAME_oneline(name, NULL, 0);
    cert.put("issuer", (std::string)(charName));

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, cert);

    return ss.str();
}

std::string Api::getDSCCertificates() {
    CertStore& certStore = CertStore::Instance();

    ptree baseTree;

    std::unordered_map<std::string, Cert>* dscList = certStore.getDSCList();

    for(std::unordered_map<std::string, Cert>::iterator it = dscList->begin(); it != dscList->end(); it++) {
        ptree cert;
        cert.push_back(std::make_pair("statusList", statusListToPtree(it->second.getStatusList())));
        cert.put("active", it->second.isCertAtive());
        cert.put("currency", it->second.getCurrencyId());
        cert.put("expirationDate", it->second.getExpirationDate());
        cert.put("rootSignature", Hexdump::vectorToHexString(it->second.getRootSignature()));
        baseTree.add_child(it->first, cert);
    }

    std::stringstream ss;
    boost::property_tree::json_parser::write_json(ss, baseTree);

    return ss.str();
}


std::string Api::addCert(std::string json, uint8_t type) {

    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    // @TODO

    return "{\"success\": false}";
}


std::string Api::removeCert(std::string json, uint8_t type) {

    if(json.empty()) {
        return "{\"success\": false, \"error\": \"empty json\"}";
    }

    std::stringstream ss(json);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ss, pt);
    Peers &peers = Peers::Instance();

    bool removedPeer = false;
    for (boost::property_tree::ptree::value_type &v : pt) {
        if (strcmp(v.first.data(), "certId") == 0) {
            std::vector<unsigned char> certId = Hexdump::hexStringToVector(v.second.data());
            CertStore& certStore = CertStore::Instance();
            Cert* cert;
            switch(type) {
                case TYPE_DSC:
                    cert = certStore.getDscCertWithCertId(certId);
                    break;
                case TYPE_CSCA:
                    cert = certStore.getCscaCertWithCertId(certId);
                    break;
                default:
                    Log(LOG_LEVEL_ERROR) << "Unknown cert type:" << type;
                    return "{\"success\": false, \"error\": \"Unknown cert typen\"}";
            }

            if(cert == nullptr) {
                Log(LOG_LEVEL_ERROR) << "Certificate with ID:" << certId << " not found";
                return "{\"success\": false, \"error\": \"Certificate not found\"}";
            }

            TxPool& txPool = TxPool::Instance();
            TxIn *txIn = new TxIn();

            UAmount inAmount;
            txIn->setAmount(inAmount);
            txIn->setNonce(cert->getNonce());
            txIn->setInAddress(certId);


            std::vector<TxIn> txIns;
            txIns.emplace_back(*txIn);

            Transaction* tx = new Transaction();
            tx->setNetwork(NET_CURRENT);
            tx->setTxIns(txIns);

            std::vector<unsigned char> txId = TransactionHelper::getTxId(tx);
            std::vector<unsigned char> privKey = Hexdump::hexStringToVector(UBIC_ROOT_PRIVATE_KEY);
            std::vector<unsigned char> signature = CreateSignature::sign(privKey, txId);

            DeactivateCertificateScript deactivateCertificateScript;
            deactivateCertificateScript.type = type;
            deactivateCertificateScript.rootCertSignature = signature;

            CDataStream s(SER_DISK, 1);
            s << deactivateCertificateScript;

            UScript script;
            script.setScript((unsigned char*)s.data(), (uint16_t)s.size());
            script.setScriptType(SCRIPT_DEACTIVATE_CERTIFICATE);
            txIn->setScript(script);

            std::vector<TxIn> txIns2;
            txIns.emplace_back(*txIn);
            tx->setTxIns(txIns2);

            txPool.appendTransaction(*tx);
        }
    }

    return "{\"success\": false}";
}
