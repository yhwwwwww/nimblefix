#pragma once

#include <cstdint>
#include <string_view>

namespace fastfix::codec::tags {

// Frame structure.
inline constexpr std::uint32_t kBeginString = 8U;
inline constexpr std::uint32_t kBodyLength = 9U;
inline constexpr std::uint32_t kCheckSum = 10U;

// Core session header.
inline constexpr std::uint32_t kMsgSeqNum = 34U;
inline constexpr std::uint32_t kMsgType = 35U;
inline constexpr std::uint32_t kPossDupFlag = 43U;
inline constexpr std::uint32_t kSenderCompID = 49U;
inline constexpr std::uint32_t kSendingTime = 52U;
inline constexpr std::uint32_t kTargetCompID = 56U;

// Optional session header and replay semantics.
inline constexpr std::uint32_t kPossResend = 97U;
inline constexpr std::uint32_t kOrigSendingTime = 122U;
inline constexpr std::uint32_t kDefaultApplVerID = 1137U;

// Session admin and control fields.
inline constexpr std::uint32_t kBeginSeqNo = 7U;
inline constexpr std::uint32_t kEndSeqNo = 16U;
inline constexpr std::uint32_t kNewSeqNo = 36U;
inline constexpr std::uint32_t kRefSeqNum = 45U;
inline constexpr std::uint32_t kText = 58U;
inline constexpr std::uint32_t kSignature = 89U;
inline constexpr std::uint32_t kEncryptMethod = 98U;
inline constexpr std::uint32_t kHeartBtInt = 108U;
inline constexpr std::uint32_t kTestReqID = 112U;
inline constexpr std::uint32_t kOnBehalfOfCompID = 115U;
inline constexpr std::uint32_t kGapFillFlag = 123U;
inline constexpr std::uint32_t kDeliverToCompID = 128U;
inline constexpr std::uint32_t kResetSeqNumFlag = 141U;
inline constexpr std::uint32_t kRefTagID = 371U;
inline constexpr std::uint32_t kRefMsgType = 372U;
inline constexpr std::uint32_t kRejectReason = 373U;
inline constexpr std::uint32_t kNextExpectedMsgSeqNum = 789U;

// Application-version negotiation.
inline constexpr std::uint32_t kApplVerID = 1128U;

// Common application and repeating-group fields.
inline constexpr std::uint32_t kAccount = 1U;
inline constexpr std::uint32_t kClOrdID = 11U;
inline constexpr std::uint32_t kOrderQty = 38U;
inline constexpr std::uint32_t kOrdType = 40U;
inline constexpr std::uint32_t kPrice = 44U;
inline constexpr std::uint32_t kSide = 54U;
inline constexpr std::uint32_t kSymbol = 55U;
inline constexpr std::uint32_t kTransactTime = 60U;
inline constexpr std::uint32_t kPartyIDSource = 447U;
inline constexpr std::uint32_t kPartyID = 448U;
inline constexpr std::uint32_t kPartyRole = 452U;
inline constexpr std::uint32_t kNoPartyIDs = 453U;
inline constexpr std::uint32_t kNoSides = 552U;
inline constexpr std::uint32_t kNoLegs = 555U;
inline constexpr std::uint32_t kLegSymbol = 600U;
inline constexpr std::uint32_t kLegTransactTime = 601U;

// Common wire prefixes for hand-written encode paths.
inline constexpr std::string_view kBeginStringPrefix = "8=";
inline constexpr std::string_view kBodyLengthPrefix = "9=";
inline constexpr std::string_view kCheckSumPrefix = "10=";
inline constexpr std::string_view kMsgSeqNumPrefix = "34=";
inline constexpr std::string_view kMsgTypePrefix = "35=";
inline constexpr std::string_view kPossDupFlagYesField = "43=Y";
inline constexpr std::string_view kSenderCompIDPrefix = "49=";
inline constexpr std::string_view kSendingTimePrefix = "52=";
inline constexpr std::string_view kTargetCompIDPrefix = "56=";
inline constexpr std::string_view kTextPrefix = "58=";
inline constexpr std::string_view kEncryptMethodPrefix = "98=";
inline constexpr std::string_view kHeartBtIntPrefix = "108=";
inline constexpr std::string_view kTestReqIDPrefix = "112=";
inline constexpr std::string_view kOnBehalfOfCompIDPrefix = "115=";
inline constexpr std::string_view kOrigSendingTimePrefix = "122=";
inline constexpr std::string_view kDeliverToCompIDPrefix = "128=";
inline constexpr std::string_view kApplVerIDPrefix = "1128=";
inline constexpr std::string_view kDefaultApplVerIDPrefix = "1137=";

[[nodiscard]] inline constexpr auto IsFrameStructureTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case kBeginString:
        case kBodyLength:
        case kCheckSum:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline constexpr auto IsStandardSessionHeaderTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case kMsgSeqNum:
        case kMsgType:
        case kPossDupFlag:
        case kSenderCompID:
        case kSendingTime:
        case kTargetCompID:
        case kPossResend:
        case kOrigSendingTime:
        case kDefaultApplVerID:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline constexpr auto IsCommonAdminTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case kBeginSeqNo:
        case kEndSeqNo:
        case kNewSeqNo:
        case kRefSeqNum:
        case kText:
        case kEncryptMethod:
        case kHeartBtInt:
        case kTestReqID:
        case kGapFillFlag:
        case kResetSeqNumFlag:
        case kNextExpectedMsgSeqNum:
        case kRefTagID:
        case kRefMsgType:
        case kRejectReason:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline constexpr auto IsSessionEnvelopeTag(std::uint32_t tag) -> bool {
    return IsFrameStructureTag(tag) || IsStandardSessionHeaderTag(tag);
}

[[nodiscard]] inline constexpr auto IsTemplateManagedHeaderTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case kMsgSeqNum:
        case kMsgType:
        case kPossDupFlag:
        case kSenderCompID:
        case kSendingTime:
        case kTargetCompID:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline constexpr auto IsEncodeManagedTag(std::uint32_t tag) -> bool {
    return IsFrameStructureTag(tag) || IsTemplateManagedHeaderTag(tag);
}

}  // namespace fastfix::codec::tags
