#include "decentralized_rng_dll.h"
#include <random>
#include <map>
#include <cstring>
#include <iostream>
#include <vector>
#include <fstream>
#include <cassert>
#include <sstream>
#include <algorithm>
#include <iomanip>

#include <locale>
#include <cctype>

#include "modules/Keccak/keccak.h"
#include "modules/shuffle_knuth.h"
#include "modules/sfmt.h"

namespace RNG
{
    using HashInfoList = std::vector<HashInfo>;
    using HashedCardsDeck = std::vector<CardHash>;

    using SeedInfoList = std::vector<SeedInfo>;
    using CardVerifyInfoList = std::vector<CardVerifyInfo>;

    struct OperatorInfo
    {
        Hash256 hash;
        Seed256 seed;
    };

    struct PlayerInfo
    {
        int playerSeatIndex;
        std::string nickname;

        Hash256 hash;
        Seed256 seed;
    };

    using PlayerInfoList = std::vector<PlayerInfo>;

    struct HandInfo
    {
        HandId              handId;
        int                 playerSeatIndex;
        OperatorInfo        operatorInfo;
        PlayerInfoList      playerInfoList;
        HashedCardsDeck     initialDeck;
        Seed256             seed;
        Hash256             hash;

        Seed256             combinedSeed;
        HashedCardsDeck     shuffledDeck;
        CardVerifyInfoList  cardVerifyInfoList;
    };

    class DecentralizedRandomNumberGenerator : public IDecentralizedRandomNumberGenerator
    {
        using TMap = std::map<HandId, HandInfo>;

    public:
        virtual const Hash256* CALL BeginHand(const BeginHandParams& params) override
        {
            // Add new HandInfo
            TMap::iterator itr = m_map.insert(TMap::value_type(params.handId, HandInfo())).first;
            if (itr == m_map.end())
            {
                return NULL;
            }

            HandInfo* handInfoPtr = &itr->second;

            handInfoPtr->handId = params.handId;
            handInfoPtr->playerSeatIndex = params.playerSeatIndex;
            handInfoPtr->initialDeck.insert(handInfoPtr->initialDeck.begin(),
                                            &params.initialCardList[0],
                                            &params.initialCardList[params.initialCardListSize]);

            for (int i = 0; i < params.nicknameListSize; i++)
            {
                const NicknameInfo& nicknameInfo = params.nicknameList[i];

                PlayerInfo newPlayerInfo;
                newPlayerInfo.playerSeatIndex = nicknameInfo.playerSeatIndex;
                newPlayerInfo.nickname = nicknameInfo.nickname;

                handInfoPtr->playerInfoList.push_back(newPlayerInfo);
            }

            // must sort by player seat index
            std::sort(handInfoPtr->playerInfoList.begin(), handInfoPtr->playerInfoList.end(), [](const PlayerInfo& a, const PlayerInfo& b) {
                    return a.playerSeatIndex < b.playerSeatIndex;
                    });

            Seed256 seed = {};
            if (params.inputSeed && params.inputSeedSize > 0) {
                std::copy_n(params.inputSeed, std::min(params.inputSeedSize, static_cast<int>(seed.size())), seed.begin());
            } else {
                std::random_device rd{};
                seed = {
                    (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(),
                    (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(),
                    (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(),
                    (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd(),
                };
            }

            handInfoPtr->seed = seed;

            Hash256 hashedSeed;
            FIPS202_SHAKE256_PURE((const unsigned char*)&seed, sizeof(seed), (unsigned char*)&hashedSeed, sizeof(hashedSeed));
            handInfoPtr->hash = hashedSeed;

            return &handInfoPtr->hash;
        }


        virtual const Seed256* CALL GetSeed(const HandId& handId, const HashInfo* hashInfoList, int hashInfoListSize) override
        {
            TMap::iterator handIt = m_map.find(handId);
            if (handIt == m_map.end())
            {
                return NULL;
            }

            HandInfo* handInfoPtr = &handIt->second;

            // set hash for operator and per Player
            for (int i = 0; i < hashInfoListSize; i++)
            {
                const HashInfo& rcvHashInfo = hashInfoList[i];
                if (-1 == rcvHashInfo.playerSeatIndex)
                {
                    // Hash generated by Operator
                    handInfoPtr->operatorInfo.hash = rcvHashInfo.hash;
                }
                else
                {
                    // Hash generated by player
                    for (auto& playerInfoIt : handInfoPtr->playerInfoList)
                    {
                        if (playerInfoIt.playerSeatIndex == rcvHashInfo.playerSeatIndex)
                        {
                            playerInfoIt.hash = rcvHashInfo.hash;
                            break;
                        }
                    }
                }
            }

            return &handIt->second.seed;
        }


        virtual void CALL AbortHand(const HandId&) override
        {
        }

        virtual Result::Enum CALL VerifyHand(const VerifyHandParams& params) override
        {
            TMap::iterator handIt = m_map.find(params.handId);
            if (handIt == m_map.end())
            {
                return Result::FAILED;
            }

            HandInfo* handInfoPtr = &handIt->second;

            // set seeds per Player
            for (int i = 0; i < params.seedInfoListSize; i++)
            {
                const SeedInfo& rcvSeedInfo = params.seedInfoList[i];
                if (-1 == rcvSeedInfo.playerSeatIndex)
                {
                    // Seed generated by Operator
                    handInfoPtr->operatorInfo.seed = rcvSeedInfo.seed;
                }
                else
                {
                    // Seed generated by Player
                    for (auto& playerInfoIt : handInfoPtr->playerInfoList)
                    {
                        if (playerInfoIt.playerSeatIndex == rcvSeedInfo.playerSeatIndex)
                        {
                            playerInfoIt.seed = rcvSeedInfo.seed;
                            break;
                        }
                    }
                }
            }

            handInfoPtr->combinedSeed = CalculateCombinedSeed(params.seedInfoList, params.seedInfoListSize);
            handInfoPtr->shuffledDeck = ShuffleDeck(handInfoPtr->initialDeck, handInfoPtr->combinedSeed);
            handInfoPtr->cardVerifyInfoList.insert(handInfoPtr->cardVerifyInfoList.begin(), &params.cardsToVerifyList[0], &params.cardsToVerifyList[params.cardsToVerifyListSize]);

            // log
            LogFull(*handInfoPtr);

            // verifying hashes and seeds
            for (const auto& playerInfoIt : handInfoPtr->playerInfoList)
            {
                const Hash256& hash = playerInfoIt.hash;
                const Seed256& seed = playerInfoIt.seed;

                if (handInfoPtr->playerSeatIndex == playerInfoIt.playerSeatIndex)
                {
                    if (handInfoPtr->hash != playerInfoIt.hash || handInfoPtr->seed != playerInfoIt.seed)
                    {
                        // RNG compromized
                        return Result::FAILED;
                    }
                }

                Hash256 calcHash;
                FIPS202_SHAKE256_PURE((const unsigned char*)(&seed), sizeof(seed), (unsigned char*)&calcHash, sizeof(calcHash));

                if (hash != calcHash)
                {
                    // RNG compromized
                    return Result::FAILED;
                }
            }

            // unlocking cards
            for (unsigned i = 0; i < handInfoPtr->cardVerifyInfoList.size(); i++)
            {
                const CardVerifyInfo& cardInfo = handInfoPtr->cardVerifyInfoList[i];

                if (!CheckCardHash(handInfoPtr->shuffledDeck[cardInfo.cardIndex], constructCardSaltAndValue(cardInfo.card))) {
                    // RNG compromized
                    return Result::FAILED;
                }
            }

            return Result::SUCCEED;
        }


    private:
        Seed256 CalculateCombinedSeed(const SeedInfo* seedInfoList, int seedInfoListSize) const
        {
            if (!seedInfoList || 0 == seedInfoListSize)
                return Seed256();

            // concatenate seeds
            std::vector<Seed256> concatenateSeeds;
            for (int i = 0; i < seedInfoListSize; i++)
            {
                concatenateSeeds.push_back(seedInfoList[i].seed);
            }

            // calculate final seed (by keccak algorithm)
            Seed256 combinedSeed;
            FIPS202_SHAKE256_PURE((const unsigned char*)&concatenateSeeds[0], sizeof(Seed256)*concatenateSeeds.size(), (unsigned char*)&combinedSeed, sizeof(combinedSeed));

            return combinedSeed;
        }


        HashedCardsDeck ShuffleDeck(const HashedCardsDeck& deck, const Seed256& seed) const
        {
            //
            // Shuffling deck using shuffle_knuth() algorithm
            //

            if (deck.empty())
            {
                return HashedCardsDeck();
            }

            HashedCardsDeck result = deck;
            shuffle_knuth(result.begin(), result.end(), Sfmt({seed.begin(), seed.end()}));
            return result;
        }


        template <typename T>
        std::string Dec2Hex(const T& object) const
        {
            // convert dec to hex
            static const char dec2hex[] = "0123456789abcdef";

            const unsigned char* bytePtr = (const unsigned char*)&object;

            std::string result;
            for (unsigned i = 0; i < sizeof(object); i++)
            {
                const unsigned char dec = bytePtr[i];

                const unsigned char value = dec / 16;
                const unsigned char remainder = dec - value * 16;

                result += dec2hex[value];
                result += dec2hex[remainder];
            }

            return result;
        }

        static const char* cardIndexToText(const char card) {
            const char szCard[] = {'2','3','4','5','6','7','8','9','T','J','Q','K','A', 0};
            const char szSuit[] = {'s','h','d','c','X',0};

            static char buf[3] = {};

            int s = (card-1) % 4;
            int c = (card-s) / 4;

            buf[0] = szCard[c];
            buf[1] = szSuit[s];
            buf[2] = '\0';

            return buf;
        }

        static std::vector<uint8_t> constructCardSaltAndValue(const SaltedCard& cardInfo) {
            std::vector<uint8_t> saltAndValue(cardInfo.salt.begin(), cardInfo.salt.end());
            saltAndValue.push_back(0);
            auto text = cardIndexToText(cardInfo.card);
            saltAndValue.insert(saltAndValue.end(), text, text+2);

            return saltAndValue;
        }

        bool CheckCardHash(const CardHash& cardHash, const std::vector<uint8_t>& saltAndValue) const
        {
            Hash256 resHash;
            FIPS202_SHAKE256_PURE(saltAndValue.data(), saltAndValue.size()*sizeof(saltAndValue[0]), resHash.data(), resHash.size()*sizeof(resHash[0]));

            return cardHash == resHash;
        }


        const CardVerifyInfo* FindCardVerifyInfo(const CardVerifyInfoList& cardVerifyInfoList, int shuffledCardIndex) const
        {
            for (unsigned i = 0; i < cardVerifyInfoList.size(); i++)
            {
                if (cardVerifyInfoList[i].cardIndex == shuffledCardIndex)
                {
                    return &cardVerifyInfoList[i];
                }
            }

            return NULL;
        }

        static std::ostream& printSeedAscii(std::ostream& os, const Seed256& b) {
            for (auto& c : b)
                os << (std::isprint(c) ? (char)c : '.');

            return os;
        }

        static std::ostream& printSalt(std::ostream& os, const std::vector<uint8_t>& b) {
            std::ios_base::fmtflags f( os.flags() );

            os << "H(";

            for (auto& c : b)
                os << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);

            os << ") | ASCII: ";

            for (auto& c : b)
                os << (std::isprint(c) ? (char)c : '.');

            os.flags(f);

            return os;
        }

        void LogFull(const HandInfo& handInfo)
        {
            const HashedCardsDeck& initialDeck = handInfo.initialDeck;

            std::stringstream logFilename;
            logFilename << "log_rng/HandId_" << handInfo.handId << ".log";
            std::ofstream outputFile(logFilename.str().c_str(), std::ofstream::out | std::ios::app);
            {
                //
                // print HandId
                //
                outputFile << "--------------------------------------------------------------------" << std::endl;
                outputFile << "HandId: " << handInfo.handId << std::endl;
                outputFile << "--------------------------------------------------------------------" << std::endl;

                //
                // print initial Deck
                //
                outputFile << "Initial hashed deck:" << std::endl;
                for (unsigned i = 0; i < initialDeck.size(); i++)
                {
                    outputFile << (i + 1 < 10 ? " " : "") << i + 1 << ". " << Dec2Hex(initialDeck[i]);
                    outputFile << std::endl;
                }

                //
                // print seeds
                //
                outputFile << std::endl;
                outputFile << "Seeds by seat index:" << std::endl;
                outputFile << "       Seed Hex Representation                                            Seed Text Representation" << std::endl;
                outputFile << "    -1 " << Dec2Hex(handInfo.operatorInfo.seed) << " | ASCII: ";
                printSeedAscii(outputFile, handInfo.operatorInfo.seed);
                outputFile << " (operator)" << std::endl;
                for (unsigned i = 0; i < handInfo.playerInfoList.size(); i++)
                {
                    const PlayerInfo& playerInfo = handInfo.playerInfoList[i];

                    outputFile << "    " << std::setw(2) << playerInfo.playerSeatIndex << " " << Dec2Hex(playerInfo.seed) << " | ASCII: ";
                    printSeedAscii(outputFile, playerInfo.seed);
                    outputFile << " (Player: " << playerInfo.nickname << ")";
                    outputFile << std::endl;
                }

                //
                // print combined seed
                //
                const Seed256& combinedSeed = handInfo.combinedSeed;

                outputFile << "    ----------------------------------------------------------------" << std::endl;
                outputFile << "    " << Dec2Hex(combinedSeed) << " (combined)" << std::endl;

                //
                // print shuffled Deck and check card hashes
                //
                const HashedCardsDeck& shuffledDeck = handInfo.shuffledDeck;

                outputFile << std::endl;
                outputFile << "Shuffled hashed deck:" << std::endl;
                outputFile << "    Card Hash                                                           "
                    << "Card Hex Representation (salt + card)                                       "
                    << "Card Text Representation" << std::endl;
                for (unsigned i = 0; i < shuffledDeck.size(); i++)
                {
                    const int initialCardIndex = std::find(initialDeck.begin(), initialDeck.end(), shuffledDeck[i]) - initialDeck.begin();
                    outputFile << (initialCardIndex+1 < 10 ? " " : "") << initialCardIndex+1 << ". " << Dec2Hex(shuffledDeck[i]);

                    const CardVerifyInfo* cardInfo = FindCardVerifyInfo(handInfo.cardVerifyInfoList, i);
                    if (cardInfo)
                    {
                        auto saltAndValue = constructCardSaltAndValue(cardInfo->card);
                        auto res = CheckCardHash(shuffledDeck[cardInfo->cardIndex], saltAndValue);

                        outputFile << " <- ";
                        printSalt(outputFile, saltAndValue);
                        outputFile << " - " << (res ? "ok" : "invalid hash");
                    }

                    outputFile << std::endl;
                }
            }
            outputFile.close();
        }

    private:
        int m_value;
        float m_valueFloat;
        TMap m_map;
    };

}//RNG

#if defined WIN32
    #define EXPORT
#else
    #define EXPORT __attribute__((visibility("default")))
#endif

EXPORT
RNG::IDecentralizedRandomNumberGenerator* CreateRNG()
{
    return new RNG::DecentralizedRandomNumberGenerator();
}

EXPORT
void DestroyRNG(RNG::IDecentralizedRandomNumberGenerator* rng)
{
    delete rng;
}
