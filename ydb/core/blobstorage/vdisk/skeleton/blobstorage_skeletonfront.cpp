#include "blobstorage_monactors.h"
#include "blobstorage_skeletonfront.h"
#include "blobstorage_skeletonerr.h"
#include "blobstorage_skeleton.h"
#include <ydb/core/blobstorage/base/blobstorage_events.h>
#include <ydb/core/blobstorage/base/utility.h>
#include <ydb/core/blobstorage/base/html.h>

#include <ydb/core/blobstorage/vdisk/common/vdisk_context.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_costmodel.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_hugeblobctx.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_pdisk_error.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_pdiskctx.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_events.h>
#include <ydb/core/blobstorage/vdisk/skeleton/skeleton_events.h>
#include <ydb/core/blobstorage/vdisk/scrub/scrub_actor.h>
#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo.h>
#include <ydb/core/blobstorage/backpressure/queue_backpressure_server.h>

#include <ydb/core/util/queue_inplace.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/base/wilson.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>

#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/actors/wilson/wilson_span.h>

#include <util/generic/set.h>
#include <util/generic/maybe.h>

using namespace NKikimrServices;

namespace NKikimr {

    ////////////////////////////////////////////////////////////////////////////
    // TEvFrontRecoveryStatus
    ////////////////////////////////////////////////////////////////////////////
    TEvFrontRecoveryStatus::TEvFrontRecoveryStatus(EPhase phase,
            NKikimrProto::EReplyStatus status,
            const TIntrusivePtr<TPDiskParams> &dsk,
            std::shared_ptr<THugeBlobCtx> hugeBlobCtx,
            TVDiskIncarnationGuid vdiskIncarnationGuid)
        : Phase(phase)
        , Status(status)
        , Dsk(dsk)
        , HugeBlobCtx(std::move(hugeBlobCtx))
        , VDiskIncarnationGuid(vdiskIncarnationGuid)
    {}

    TEvFrontRecoveryStatus::~TEvFrontRecoveryStatus() = default;

    namespace {

        // Helper function to extract protobuf's Record from event and push it to callback
        template<typename Func>
        void ApplyToRecord(IEventHandle& event, Func&& callback) {
            switch (event.GetTypeRewrite()) {
#define EVENT_TYPE(EVENT) case TEvBlobStorage::EVENT::EventType: callback(static_cast<TEvBlobStorage::EVENT&>(*event.GetBase()).Record); return;
                EVENT_TYPE(TEvVMovedPatch)
                EVENT_TYPE(TEvVPatchStart)
                EVENT_TYPE(TEvVPatchDiff)
                EVENT_TYPE(TEvVPatchXorDiff)
                EVENT_TYPE(TEvVPut)
                EVENT_TYPE(TEvVMultiPut)
                EVENT_TYPE(TEvVGet)
                EVENT_TYPE(TEvVBlock)
                EVENT_TYPE(TEvVGetBlock)
                EVENT_TYPE(TEvVCollectGarbage)
                EVENT_TYPE(TEvVGetBarrier)
#undef EVENT_TYPE
            }
            Y_FAIL("unsupported event type");
        }

        struct TUpdateInQueueTime {
            TDuration InQueue;

            TUpdateInQueueTime(TDuration inQueue)
                : InQueue(inQueue)
            {}

            template<typename TRecord>
            void operator ()(TRecord& record) const {
                auto& msgQoS = *record.MutableMsgQoS();
                auto& execTimeStats = *msgQoS.MutableExecTimeStats();
                execTimeStats.SetInQueue(InQueue.GetValue());
            }
        };

        ////////////////////////////////////////////////////////////////////////////
        // TRecord for delayed queue
        ////////////////////////////////////////////////////////////////////////////
        struct TRecord {
            std::unique_ptr<IEventHandle> Ev;
            TInstant ReceivedTime;
            TInstant Deadline; // after this deadline we will not process this request
            ui32 ByteSize;
            NBackpressure::TMessageId MsgId;
            ui64 Cost;
            NKikimrBlobStorage::EVDiskQueueId ExtQueueId;
            NBackpressure::TQueueClientId ClientId;
            TActorId ActorId;
            NWilson::TSpan Span;

            TRecord() = default;

            TRecord(std::unique_ptr<IEventHandle> ev, TInstant now, ui32 recByteSize, const NBackpressure::TMessageId &msgId,
                    ui64 cost, TInstant deadline, NKikimrBlobStorage::EVDiskQueueId extQueueId,
                    const NBackpressure::TQueueClientId& clientId, TString name)
                : Ev(std::move(ev))
                , ReceivedTime(now)
                , Deadline(deadline)
                , ByteSize(recByteSize)
                , MsgId(msgId)
                , Cost(cost)
                , ExtQueueId(extQueueId)
                , ClientId(clientId)
                , ActorId(Ev->Sender)
                , Span(TWilson::VDiskTopLevel, std::move(Ev->TraceId), "VDisk.SkeletonFront.Queue")
            {
                Span.Attribute("QueueName", std::move(name));
                Ev->TraceId = Span.GetTraceId();
            }
        };

        using TMyQueueBackpressure = NBackpressure::TQueueBackpressure<NBackpressure::TQueueClientId>;
        using TWindowStatus = TMyQueueBackpressure::TWindowStatus;
        using TWindowStatusOpt = TMaybe<TWindowStatus>;
        using TFeedback = TMyQueueBackpressure::TFeedback;

        void SendWindowChange(const TActorContext &ctx, const TWindowStatus &wstatus, NKikimrBlobStorage::EVDiskQueueId queueId) {
            ctx.Send(wstatus.ActorId, new TEvBlobStorage::TEvVWindowChange(queueId, wstatus));
        }

        ////////////////////////////////////////////////////////////////////////////
        // TIntQueueClass -- delayed queue
        ////////////////////////////////////////////////////////////////////////////
        class TIntQueueClass {
            using TQueueType = TQueueInplace<TRecord, 4096>;

        private:
            std::unique_ptr<TQueueType, TQueueType::TCleanDestructor> Queue;
            ui64 InFlightCount;
            ui64 InFlightCost;
            ui64 InFlightBytes;
            ui64 DelayedCount;
            ui64 DelayedBytes;
            ui64 Deadlines;
            const ui64 MaxInFlightCount;
            const ui64 MaxInFlightCost;
        public:
            const NKikimrBlobStorage::EVDiskInternalQueueId IntQueueId;
            const TString Name;

        private:
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontInFlightCount;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontInFlightCost;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontInFlightBytes;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontDelayedCount;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontDelayedBytes;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontCostProcessed;

            bool CanSendToSkeleton(ui64 cost) const {
                bool inFlightCond = InFlightCount < MaxInFlightCount;
                // for one query we can exceed MaxInFlightCost
                bool costCond = (InFlightCount == 0) || ((InFlightCost + cost) < MaxInFlightCost);
                return inFlightCond && costCond;
            }

        public:
            TIntQueueClass(
                    const NKikimrBlobStorage::EVDiskInternalQueueId intQueueId,
                    const TString &name,
                    ui64 maxInFlightCount,
                    ui64 maxInFlightCost,
                    TIntrusivePtr<::NMonitoring::TDynamicCounters> skeletonFrontGroup)
                : Queue(new TQueueType())
                , InFlightCount(0)
                , InFlightCost(0)
                , InFlightBytes(0)
                , DelayedCount(0)
                , DelayedBytes(0)
                , Deadlines(0)
                , MaxInFlightCount(maxInFlightCount)
                , MaxInFlightCost(maxInFlightCost)
                , IntQueueId(intQueueId)
                , Name(name)
                , SkeletonFrontInFlightCount(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/InFlightCount", false))
                , SkeletonFrontInFlightCost(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/InFlightCost", false))
                , SkeletonFrontInFlightBytes(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/InFlightBytes", false))
                , SkeletonFrontDelayedCount(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/DelayedCount", false))
                , SkeletonFrontDelayedBytes(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/DelayedBytes", false))
                , SkeletonFrontCostProcessed(skeletonFrontGroup->GetCounter("SkeletonFront/" + Name + "/CostProcessed", true))
            {}

            ui64 GetSize() const {
                return Queue->GetSize();
            }

            template<typename TFront>
            void Enqueue(const TActorContext &ctx, ui32 recByteSize, std::unique_ptr<IEventHandle> converted,
                         const NBackpressure::TMessageId &msgId, ui64 cost, const TInstant &deadline,
                         NKikimrBlobStorage::EVDiskQueueId extQueueId, TFront& front,
                         const NBackpressure::TQueueClientId& clientId) {
                Y_UNUSED(front);
                if (!Queue->Head() && CanSendToSkeleton(cost)) {
                    // send to Skeleton for further processing
                    ctx.ExecutorThread.Send(converted.release());
                    ++InFlightCount;
                    InFlightCost += cost;
                    InFlightBytes += recByteSize;

                    ++*SkeletonFrontInFlightCount;
                    *SkeletonFrontInFlightCost += cost;
                    *SkeletonFrontInFlightBytes += recByteSize;
                } else {
                    // enqueue
                    ++DelayedCount;
                    DelayedBytes += recByteSize;

                    ++*SkeletonFrontDelayedCount;
                    *SkeletonFrontDelayedBytes += recByteSize;

                    TInstant now = TAppData::TimeProvider->Now();
                    Queue->Push(TRecord(std::move(converted), now, recByteSize, msgId, cost, deadline, extQueueId,
                        clientId, Name));
                }
            }

            template<typename TFront>
            void DropWithError(const TActorContext& ctx, TFront& front) {
                ProcessNext(ctx, front, true);
            }

        private:
            template <class TFront>
            void ProcessNext(const TActorContext &ctx, TFront &front, bool forceError) {
                // we can send next element to Skeleton if any
                while (TRecord *rec = Queue->Head()) {
                    const ui64 cost = rec->Cost;
                    if (CanSendToSkeleton(cost) || forceError) {
                        ui32 recByteSize = rec->ByteSize;
                        Y_VERIFY_DEBUG(DelayedCount > 0 && DelayedBytes >= recByteSize);

                        --DelayedCount;
                        DelayedBytes -= recByteSize;
                        --*SkeletonFrontDelayedCount;
                        *SkeletonFrontDelayedBytes -= recByteSize;

                        // update in-queue duration for this query
                        TInstant now = TAppData::TimeProvider->Now();
                        TDuration inQueue = now - rec->ReceivedTime;
                        ApplyToRecord(*rec->Ev, TUpdateInQueueTime(inQueue));

                        // trace end of in-queue span
                        rec->Span.EndOk();

                        if (forceError) {
                            front.GetExtQueue(rec->ExtQueueId).DroppedWithError(ctx, rec, now, front);
                        } else if (now >= rec->Deadline) {
                            ++Deadlines;
                            front.GetExtQueue(rec->ExtQueueId).DeadlineHappened(ctx, rec, now, front);
                        } else {
                            ctx.ExecutorThread.Send(rec->Ev.release());

                            ++InFlightCount;
                            InFlightCost += cost;
                            InFlightBytes += recByteSize;

                            ++*SkeletonFrontInFlightCount;
                            *SkeletonFrontInFlightCost += cost;
                            *SkeletonFrontInFlightBytes += recByteSize;
                        }
                        Queue->Pop();
                    } else {
                        break; // stop sending requests to skeleton
                    }
                }
            }

        public:
            template <class TFront>
            void Completed(const TActorContext &ctx, const TVMsgContext &msgCtx, TFront &front) {
                Y_VERIFY(InFlightCount >= 1 && InFlightBytes >= msgCtx.RecByteSize && InFlightCost >= msgCtx.Cost,
                         "IntQueueId# %s InFlightCount# %" PRIu64 " InFlightBytes# %" PRIu64
                         " InFlightCost# %" PRIu64 " msgCtx# %s Deadlines# %" PRIu64,
                         NKikimrBlobStorage::EVDiskInternalQueueId_Name(IntQueueId).data(),
                         InFlightCount, InFlightBytes, InFlightCost, msgCtx.ToString().data(), Deadlines);

                --InFlightCount;
                InFlightCost -= msgCtx.Cost;
                InFlightBytes -= msgCtx.RecByteSize;

                --*SkeletonFrontInFlightCount;
                *SkeletonFrontInFlightCost -= msgCtx.Cost;
                *SkeletonFrontInFlightBytes -= msgCtx.RecByteSize;
                *SkeletonFrontCostProcessed += msgCtx.Cost;

                ProcessNext(ctx, front, false);
            }

            // enumeration of parameters we take into account to diagnose overloading
            enum EParam {
                EInFlightCount,
                EInFlightCost,
                EInFlightBytes,
                EDelayedCount,
                EDelayedBytes,
                EMax
            };

            // Calculate signal light
            NKikimrWhiteboard::EFlag CalculateSignalLight(ui64 val,
                                                          ui64 maxVal,
                                                          NKikimrWhiteboard::EFlag warnLight) const {
                if (val > maxVal) {
                    // warning mode
                    return warnLight;
                } else {
                    // ordinary mode
                    return NKikimrWhiteboard::EFlag::Green;
                }
            }

            // get light for every param
            NKikimrWhiteboard::EFlag GetLight(EParam param) const {
                const auto yellow = NKikimrWhiteboard::EFlag::Yellow;
                const auto red = NKikimrWhiteboard::EFlag::Red;
                switch (param) {
                    case EInFlightCount: return CalculateSignalLight(InFlightCount, MaxInFlightCount / 2, yellow);
                    case EInFlightCost:  return CalculateSignalLight(InFlightCost, MaxInFlightCost / 2, yellow);
                    case EInFlightBytes: return CalculateSignalLight(InFlightBytes, Max<ui64>(), yellow);
                    case EDelayedCount:  return CalculateSignalLight(DelayedCount, 0, red);
                    case EDelayedBytes:  return CalculateSignalLight(DelayedBytes, 0, red);
                    default: Y_FAIL("Unexpected param");
                }
            }

            // get cumulative light for the whole queue
            NKikimrWhiteboard::EFlag GetCumulativeLight() const {
                auto light = NKikimrWhiteboard::EFlag::Green;
                for (int i = EInFlightCount; i < int(EMax); ++i) {
                    light = Max(light, GetLight(EParam(i)));
                }
                return light;
            }

            // Output one record; if current value is too large output in some warning color.
            // val -- current value
            // if val is greater than maxVal, output current record in 'light' color
            void OutputRecord(const char *name,
                              IOutputStream &str,
                              ui64 val,
                              NKikimrWhiteboard::EFlag light) const {
                if (light != NKikimrWhiteboard::EFlag::Green) {
                    // warning mode
                    auto s = Sprintf("%s: %" PRIu64, name, val);
                    THtmlLightSignalRenderer(light, s).Output(str);
                } else {
                    // ordinary mode (output w/o any color)
                    str << name << ": " << val;
                }
                str << "<br>";
            }

            TString GenerateHtmlState() const {
                // NOTE: warning policy:
                // 1. For InFlightCount and InFlightCost we output them in yellow, if
                //    queue size if half of maximun queue size
                // 2. For DelayedCount and DelayedBytes we output them in red if they
                //    are different from zero
                TStringStream str;
                str << "\n";
                HTML(str) {
                    DIV_CLASS("panel panel-default") {
                        DIV_CLASS("panel-heading") {
                            SMALL() {
                                STRONG() {str << "INT: ";}
                                str << Name;
                            }
                        }
                        DIV_CLASS("panel-body") {
                            OutputRecord("InFlightCount", str, InFlightCount, GetLight(EInFlightCount));
                            OutputRecord("InFlightCost", str, InFlightCost, GetLight(EInFlightCost));
                            OutputRecord("InFlightBytes", str, InFlightBytes, GetLight(EInFlightBytes));
                            OutputRecord("DelayedCount", str, DelayedCount, GetLight(EDelayedCount));
                            OutputRecord("DelayedBytes", str, DelayedBytes, GetLight(EDelayedBytes));
                        }
                    }
                }
                str << "\n";
                return str.Str();
            }
        };


        ////////////////////////////////////////////////////////////////////////////
        // TExtQueueClass -- delayed queue
        ////////////////////////////////////////////////////////////////////////////
        class TExtQueueClass {
        private:
            std::unique_ptr<TMyQueueBackpressure> QueueBackpressure;
            NKikimrBlobStorage::EVDiskQueueId ExtQueueId;
            TString Name;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontDeadline;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontOverflow;
            ::NMonitoring::TDynamicCounters::TCounterPtr SkeletonFrontIncorrectMsgId;


            void NotifyOtherClients(const TActorContext &ctx, const TFeedback &feedback) {
                for (const auto &x : feedback.second) {
                    SendWindowChange(ctx, x, ExtQueueId);
                }
            }

            void OutputBadFeedback(const TFeedback &feedback) const {
                TStringStream ss;
                ss << "Feedback: ";
                feedback.Output(ss);
                ss << "\nQueueBackpressureState: ";
                TInstant now = TAppData::TimeProvider->Now();
                QueueBackpressure->Output(ss, now);
                Cerr << "\n\n" << ss.Str() << "\n\n";
            }

        public:
            TExtQueueClass(NKikimrBlobStorage::EVDiskQueueId extQueueId, const TString &name, ui64 totalCost,
                           bool checkMsgId, TIntrusivePtr<::NMonitoring::TDynamicCounters> skeletonFrontGroup,
                           const TIntrusivePtr<TVDiskConfig>& config)
                : QueueBackpressure()
                , ExtQueueId(extQueueId)
                , Name(name)
                , SkeletonFrontDeadline(skeletonFrontGroup->GetCounter("SkeletonFront/" + name + "/Deadline", true))
                , SkeletonFrontOverflow(skeletonFrontGroup->GetCounter("SkeletonFront/" + name + "/Overflow", true))
                , SkeletonFrontIncorrectMsgId(skeletonFrontGroup->GetCounter("SkeletonFront/" + name + "/IncorrectMsgId", true))
            {
                // recalculate window percents to cost
                ui64 costChangeToRecalculate = totalCost * config->WindowCostChangeToRecalculatePercent / 100;
                ui64 minLowWatermark         = totalCost * config->WindowMinLowWatermarkPercent         / 100;
                ui64 maxLowWatermark         = totalCost * config->WindowMaxLowWatermarkPercent         / 100;
                ui64 costChangeUntilFrozen   = totalCost * config->WindowCostChangeUntilFrozenPercent   / 100;
                ui64 costChangeUntilDeath    = totalCost * config->WindowCostChangeUntilDeathPercent    / 100;

                ui64 percentThreshold = config->WindowPercentThreshold;

                QueueBackpressure = std::make_unique<TMyQueueBackpressure>(checkMsgId, totalCost, costChangeToRecalculate,
                    minLowWatermark, maxLowWatermark, percentThreshold, costChangeUntilFrozen, costChangeUntilDeath,
                    config->WindowTimeout);
            }

            std::optional<NBackpressure::TMessageId> GetExpectedMsgId(const TActorId& actorId) const {
                return QueueBackpressure->GetExpectedMsgId(actorId);
            }

            // return true if enqueed, false otherwise
            template <class TFront>
            std::unique_ptr<IEventHandle> Enqueue(const TActorContext &ctx, std::unique_ptr<IEventHandle> converted,
                                           const NBackpressure::TMessageId &msgId, ui64 cost, TFront &front,
                                           const NBackpressure::TQueueClientId& clientId) {
                TInstant now = TAppData::TimeProvider->Now();
                auto feedback = QueueBackpressure->Push(clientId, converted->Sender, msgId, cost, now);
                NotifyOtherClients(ctx, feedback);
                if (!feedback.Good()) {
#ifdef VERBOSE
                    OutputBadFeedback(feedback);
#endif
                    TString errorReason;
                    NKikimrProto::EReplyStatus status = NKikimrProto::TRYLATER_SIZE;
                    if (feedback.first.Status == NKikimrBlobStorage::TWindowFeedback::IncorrectMsgId) {
                        // special status to distinguish between incorrect message id and queue overflow
                        status = NKikimrProto::TRYLATER;
                        errorReason = "incorrect message id";
                        ++*SkeletonFrontIncorrectMsgId;
                    } else {
                        // overflow
                        errorReason = "queue overflow";
                        ++*SkeletonFrontOverflow;
                    }
                    front.ReplyFunc(std::exchange(converted, nullptr), ctx, status, errorReason, now, feedback.first);
                }
                return converted;
            }

            template <class TFront>
            void DeadlineHappened(const TActorContext &ctx, TRecord *rec, TInstant now, TFront &front) {
                ++*SkeletonFrontDeadline;
                auto feedback = QueueBackpressure->Processed(rec->ActorId, rec->MsgId, rec->Cost, now);
                front.ReplyFunc(std::move(rec->Ev), ctx, NKikimrProto::DEADLINE, "deadline exceeded", now, feedback.first);
                NotifyOtherClients(ctx, feedback);
            }

            template <class TFront>
            void DroppedWithError(const TActorContext &ctx, TRecord *rec, TInstant now, TFront &front) {
                auto feedback = QueueBackpressure->Processed(rec->ActorId, rec->MsgId, rec->Cost, now);
                front.ReplyFunc(std::move(rec->Ev), ctx, NKikimrProto::ERROR, "error state", now, feedback.first);
            }

            void DisconnectClients(const TActorContext& ctx, const TActorId& serviceId) {
                auto callback = [&](const auto& winp) {
                    ctx.Send(winp->ActorId, new TEvBlobStorage::TEvVWindowChange(ExtQueueId,
                        TEvBlobStorage::TEvVWindowChange::DropConnection));

                    // small hack for older versions of BS_QUEUE that do not support DropConnection flag
                    ctx.Send(new IEventHandle(winp->ActorId, serviceId, new TEvents::TEvUndelivered(0,
                        TEvents::TEvUndelivered::ReasonActorUnknown)));
                };
                QueueBackpressure->ForEachWindow(callback);
            }

            void Completed(const TActorContext &ctx, const TVMsgContext &msgCtx, IEventBase *ev) {
                TInstant now = TAppData::TimeProvider->Now();
                Y_VERIFY(msgCtx.ActorId);
                auto feedback = QueueBackpressure->Processed(msgCtx.ActorId, msgCtx.MsgId, msgCtx.Cost, now);
                NKikimrBlobStorage::TMsgQoS *msgQoS = nullptr;
                switch (ev->Type()) {
#define UPDATE_WINDOW_STATUS(TYPE) case TYPE::EventType: msgQoS = static_cast<TYPE&>(*ev).Record.MutableMsgQoS(); break;
                    // all message types that have MsgQoS structure
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVMovedPatchResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVPatchFoundParts)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVPatchXorDiffResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVPatchResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVPutResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVMultiPutResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVGetResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVBlockResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVGetBlockResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVCollectGarbageResult)
                    UPDATE_WINDOW_STATUS(TEvBlobStorage::TEvVGetBarrierResult)
#undef UPDATE_WINDOW_STATUS
                }
                Y_VERIFY(msgQoS);
                feedback.first.Serialize(*msgQoS->MutableWindow());
                NotifyOtherClients(ctx, feedback);
            }

            TString GenerateHtmlState() const {
                TStringStream str;
                str << "\n";
                HTML(str) {
                    DIV_CLASS("panel panel-default") {
                        DIV_CLASS("panel-heading") {
                            SMALL() {
                                STRONG() {str << "EXT: ";}
                                str << Name;
                            }
                        }
                        DIV_CLASS("panel-body") {
                            str << "Deadline: " << SkeletonFrontDeadline->Val() << "<br>";
                            str << "Overflow: " << SkeletonFrontOverflow->Val() << "<br>";
                            str << "IncorrectMsgId: " << SkeletonFrontIncorrectMsgId->Val() << "<br>";
                        }
                    }
                }
                str << "\n";


                return str.Str();
            }
        };

    } // namespace

    ////////////////////////////////////////////////////////////////////////////
    // VDISK FRONT
    ////////////////////////////////////////////////////////////////////////////
    /*

     SkeletonFront implements:
     * External queues to internal queues multiplexing/demultiplexing
     * Barrier between Skeleton (allow to pass only limited number of messages)
     * Backpressure on external queues

     External Queues                                          Internal Queues
     ===============              +--+      +--+              ===============
       TabletLog     ---------->  |  |      |  |  ---------->   IntPutLog
       AsyncBlob     ---------->  |  | MIMO |  |  ---------->   IntPutHugeBackground
       UserData      ---------->  |  |      |  |  ---------->   IntPutHugeForeground
       GetAsync      ---------->  |  |      |  |  ---------->   IntGetAsync
       GetFast       ---------->  |  |      |  |  ---------->   IntGetFast
       GetDiscover   ---------->  |  |      |  |  ---------->   IntGetDiscover
       GetLow        ---------->  |  |      |  |  ---------->   IntLowRead
                                  +--+      +--+
     */
    class TSkeletonFront : public TActorBootstrapped<TSkeletonFront> {
        friend class TActorBootstrapped<TSkeletonFront>;
        friend class TIntQueueClass;

        TVDiskContextPtr VCtx;
        TIntrusivePtr<TVDiskConfig> Config;
        TIntrusivePtr<TBlobStorageGroupInfo> GInfo;
        std::shared_ptr<TBlobStorageGroupInfo::TTopology> Top;
        TVDiskID SelfVDiskId;
        TActorId SkeletonId;
        TIntrusivePtr<::NMonitoring::TDynamicCounters> VDiskCounters;
        TIntrusivePtr<::NMonitoring::TDynamicCounters> SkeletonFrontGroup;
        ::NMonitoring::TDynamicCounters::TCounterPtr AccessDeniedMessages;
        std::unique_ptr<TIntQueueClass> IntQueueAsyncGets;
        std::unique_ptr<TIntQueueClass> IntQueueFastGets;
        std::unique_ptr<TIntQueueClass> IntQueueDiscover;
        std::unique_ptr<TIntQueueClass> IntQueueLowGets;
        std::unique_ptr<TIntQueueClass> IntQueueLogPuts;
        std::unique_ptr<TIntQueueClass> IntQueueHugePutsForeground;
        std::unique_ptr<TIntQueueClass> IntQueueHugePutsBackground;
        TExtQueueClass ExtQueueAsyncGets;
        TExtQueueClass ExtQueueFastGets;
        TExtQueueClass ExtQueueDiscoverGets;
        TExtQueueClass ExtQueueLowGets;
        TExtQueueClass ExtQueueTabletLogPuts;
        TExtQueueClass ExtQueueAsyncBlobPuts;
        TExtQueueClass ExtQueueUserDataPuts;
        std::unique_ptr<TCostModel> CostModel;
        TActiveActors ActiveActors;
        NMonGroup::TReplGroup ReplMonGroup;
        NMonGroup::TSyncerGroup SyncerMonGroup;
        NMonGroup::TVDiskStateGroup VDiskMonGroup;
        TVDiskIncarnationGuid VDiskIncarnationGuid;
        bool HasUnreadableBlobs = false;

        ////////////////////////////////////////////////////////////////////////
        // NOTIFICATIONS
        ////////////////////////////////////////////////////////////////////////
        using TNotificationIDs = TSet<TActorId>;
        TNotificationIDs NotificationIDs;

        template <class T>
        void NotifyIfNotReady(T &ev, const TActorContext &ctx) {
            Y_UNUSED(ctx);
            if (ev->Get()->Record.GetNotifyIfNotReady()) {
                NotificationIDs.insert(ev->Sender);
            }
        }

        void SendNotifications(const TActorContext &ctx) {
            for (const auto &id : NotificationIDs)
                ctx.Send(id, new TEvBlobStorage::TEvVReadyNotify());
            NotificationIDs.clear();
        }

        void SetupMonitoring(const TActorContext &ctx) {
            TAppData *appData = AppData(ctx);
            Y_VERIFY(appData);
            auto mon = appData->Mon;
            if (mon) {
                NMonitoring::TIndexMonPage *actorsMonPage = mon->RegisterIndexPage("actors", "Actors");
                NMonitoring::TIndexMonPage *vdisksMonPage = actorsMonPage->RegisterIndexPage("vdisks", "VDisks");

                const auto &bi = Config->BaseInfo;
                TString path = Sprintf("vdisk%09" PRIu32 "_%09" PRIu32, bi.PDiskId, bi.VDiskSlotId);
                TString name = Sprintf("%s VDisk%09" PRIu32 "_%09" PRIu32,
                                      VCtx->VDiskLogPrefix.data(), bi.PDiskId, bi.VDiskSlotId);
                mon->RegisterActorPage(vdisksMonPage, path, name, false, ctx.ExecutorThread.ActorSystem, ctx.SelfID);
            }
        }

        void Bootstrap(const TActorContext &ctx) {
            const auto& baseInfo = Config->BaseInfo;
            VCtx = MakeIntrusive<TVDiskContext>(ctx.SelfID, GInfo->PickTopology(), VDiskCounters, SelfVDiskId,
                        ctx.ExecutorThread.ActorSystem, baseInfo.DeviceType, baseInfo.DonorMode,
                        baseInfo.ReplPDiskReadQuoter, baseInfo.ReplPDiskWriteQuoter, baseInfo.ReplNodeRequestQuoter,
                        baseInfo.ReplNodeResponseQuoter);

            // create IntQueues
            IntQueueAsyncGets = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntGetAsync,
                    "AsyncGets",
                    Config->SkeletonFrontGets_MaxInFlightCount,
                    Config->SkeletonFrontGets_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueFastGets = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntGetFast,
                    "FastGets",
                    Config->SkeletonFrontGets_MaxInFlightCount,
                    Config->SkeletonFrontGets_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueDiscover = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntGetDiscover,
                    "DiscoverGets",
                    Config->SkeletonFrontDiscover_MaxInFlightCount,
                    Config->SkeletonFrontDiscover_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueLowGets = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntLowRead,
                    "FastGets",
                    Config->SkeletonFrontGets_MaxInFlightCount,
                    Config->SkeletonFrontGets_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueLogPuts = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntPutLog,
                    "LogPuts",
                    Config->SkeletonFrontLogPuts_MaxInFlightCount,
                    Config->SkeletonFrontLogPuts_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueHugePutsForeground = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntPutHugeForeground,
                    "HugePutsForeground",
                    Config->SkeletonFrontHugePuts_MaxInFlightCount,
                    Config->SkeletonFrontHugePuts_MaxInFlightCost,
                    SkeletonFrontGroup);
            IntQueueHugePutsBackground = std::make_unique<TIntQueueClass>(
                    NKikimrBlobStorage::EVDiskInternalQueueId::IntPutHugeBackground,
                    "HugePutsBackground",
                    Config->SkeletonFrontHugePuts_MaxInFlightCount,
                    Config->SkeletonFrontHugePuts_MaxInFlightCost,
                    SkeletonFrontGroup);

            UpdateWhiteboard(ctx);

            // create and run skeleton
            SkeletonId = ctx.Register(CreateVDiskSkeleton(Config, GInfo, ctx.SelfID, VCtx));
            ActiveActors.Insert(SkeletonId);

            SetupMonitoring(ctx);
            Become(&TThis::StateLocalRecoveryInProgress);
        }

        void Handle(TEvFrontRecoveryStatus::TPtr &ev, const TActorContext &ctx) {
            const auto &msg = ev->Get();
            VDiskIncarnationGuid = msg->VDiskIncarnationGuid;
            if (msg->Status != NKikimrProto::OK) {
                Become(&TThis::StateDatabaseError);
                SendNotifications(ctx);
            } else {
                switch (msg->Phase) {
                    case TEvFrontRecoveryStatus::LocalRecoveryDone:
                    {
                        Become(&TThis::StateSyncGuidRecoveryInProgress);
                        TBlobStorageGroupType type = (GInfo ? GInfo->Type : TErasureType::ErasureNone);
                        CostModel = std::make_unique<TCostModel>(msg->Dsk->SeekTimeUs, msg->Dsk->ReadSpeedBps,
                            msg->Dsk->WriteSpeedBps, msg->Dsk->ReadBlockSize, msg->Dsk->WriteBlockSize,
                            msg->HugeBlobCtx->MinREALHugeBlobInBytes, type);
                        break;
                    }
                    case TEvFrontRecoveryStatus::SyncGuidRecoveryDone:
                        Become(&TThis::StateFunc);
                        SendNotifications(ctx);
                        break;
                    default: Y_FAIL("Unexpected case");
                }
            }
        }

        static NKikimrWhiteboard::EFlag ToLightSignal(NKikimrWhiteboard::EVDiskState st) {
            switch (st) {
                case NKikimrWhiteboard::EVDiskState::Initial: return NKikimrWhiteboard::EFlag::Yellow;
                case NKikimrWhiteboard::EVDiskState::LocalRecoveryError: return NKikimrWhiteboard::EFlag::Red;
                case NKikimrWhiteboard::EVDiskState::SyncGuidRecovery: return NKikimrWhiteboard::EFlag::Yellow;
                case NKikimrWhiteboard::EVDiskState::SyncGuidRecoveryError: return NKikimrWhiteboard::EFlag::Red;
                case NKikimrWhiteboard::EVDiskState::OK: return NKikimrWhiteboard::EFlag::Green;
                default: return NKikimrWhiteboard::EFlag::Red;
            }
        }

        TString GenerateHtmlStateForGlobalVDiskState() const {
            TStringStream str;
            HTML(str) {
                DIV_CLASS("panel panel-info") {
                    DIV_CLASS("panel-heading") {
                        str << "VDisk State";
                    }
                    DIV_CLASS("panel-body") {
                        TABLE_CLASS("table table-condensed") {
                            TABLEHEAD() {
                                TABLER() {
                                    TABLEH() {str << "Component";}
                                    TABLEH() {str << "State";}
                                }
                            }
                            TABLEBODY() {
                                TABLER() {
                                    auto v = VDiskMonGroup.VDiskState();
                                    auto s = NKikimrWhiteboard::EVDiskState_Name(v);
                                    auto light = ToLightSignal(v);
                                    TABLED() {str << "VDisk";}
                                    TABLED() {THtmlLightSignalRenderer(light, s).Output(str);}
                                }
                                TABLER() {
                                    auto state = VDiskMonGroup.VDiskLocalRecoveryState().Val();
                                    auto s = TDbMon::TDbLocalRecovery::StateToStr(state);
                                    auto light = TDbMon::TDbLocalRecovery::StateToLight(state);
                                    TABLED() {str << "VDisk LocalDb Recovery";}
                                    TABLED() {THtmlLightSignalRenderer(light, s).Output(str);}
                                }
                                if (VDiskMonGroup.VDiskState() == NKikimrWhiteboard::PDiskError) {
                                    TABLER() {
                                        TABLED() {str << "Error Details";}
                                        TABLED() {
                                            str << "PDisk reported error: "
                                                << TPDiskErrorState::StateToString(VCtx->GetPDiskErrorState());
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            return str.Str();
        }

        TString GenerateHtmlStateForSkeletonFrontActor(const TActorContext &ctx) const {
            constexpr ui32 threshold = 10000u;
            std::pair<ui32, ui32> actorQueues = ctx.CountMailboxEvents(threshold);

            TStringStream str;
            HTML(str) {
                DIV_CLASS("panel panel-default") {
                    DIV_CLASS("panel-heading") {
                        str << "SkeletonFront Actor";
                    }
                    DIV_CLASS("panel-body") {
                        TABLE_CLASS ("table table-condensed") {
                            TABLEHEAD() {
                                TABLER() {
                                    TABLEH() {str << "Queues";}
                                    TABLEH() {str << "Size";}
                                }
                            }
                            TABLEBODY() {
                                TABLER() {
                                    TABLED() {str << "ActorQueue";}
                                    TABLED() {
                                        if (actorQueues.first >= threshold)
                                            str << "More than " << threshold;
                                        else
                                            str << actorQueues.first;
                                    }
                                }
                                TABLER() {
                                    TABLED() {str << "MailboxQueue";}
                                    TABLED() {
                                        if (actorQueues.second >= threshold)
                                            str << "More than " << threshold;
                                        else
                                            str << actorQueues.second;
                                    }
                                }
                                TABLER() {
                                    TABLED() {str << "ElapsedTicksAsSeconds";}
                                    TABLED() {str << GetElapsedTicksAsSeconds();}
                                }
                                TABLER() {
                                    TABLED() {str << "HandledEvents";}
                                    TABLED() {str << GetHandledEvents();}
                                }
                            }
                        }
                    }
                }
            }
            return str.Str();
        }

        TString GenerateHtmlState(const TActorContext &ctx) const {
            TStringStream str;
            str << "\n";
            HTML(str) {
                DIV_CLASS("panel panel-info") {
                    DIV_CLASS("panel-heading") {
                        str << "SkeletonFront";
                    }
                    DIV_CLASS("panel-body") {
                        DIV_CLASS("row") {
                            // global VDisk state and SkeletonFront State
                            DIV_CLASS("col-md-6") {str << GenerateHtmlStateForGlobalVDiskState(); }
                            DIV_CLASS("col-md-6") {str << GenerateHtmlStateForSkeletonFrontActor(ctx); }
                        }
                        DIV_CLASS("row") {
                            // int queues
                            DIV_CLASS("col-md-2") {str << IntQueueLogPuts->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueHugePutsForeground->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueHugePutsBackground->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueAsyncGets->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueFastGets->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueDiscover->GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << IntQueueLowGets->GenerateHtmlState();}
                        }
                        DIV_CLASS("row") {
                            // ext queues
                            DIV_CLASS("col-md-2") {str << ExtQueueTabletLogPuts.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueAsyncBlobPuts.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueUserDataPuts.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueAsyncGets.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueFastGets.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueDiscoverGets.GenerateHtmlState();}
                            DIV_CLASS("col-md-2") {str << ExtQueueLowGets.GenerateHtmlState();}
                            // uses column wrapping (sum is greater than 12)
                        }
                    }
                }
            }

            return str.Str();
        }

        ////////////////////////////////////////////////////////////////////////
        // WHITEBOARD SECTOR
        // Update Whiteboard with the current status
        ////////////////////////////////////////////////////////////////////////
        void UpdateWhiteboard(const TActorContext &ctx, bool schedule = true) {
            // out of space
            const auto outOfSpaceFlags = VCtx->GetOutOfSpaceState().LocalWhiteboardFlag();
            // skeleton state
            const auto state = VDiskMonGroup.VDiskState();
            // replicated?
            bool replicated = !ReplMonGroup.ReplUnreplicatedVDisks() && !HasUnreadableBlobs;
            bool unreplicatedPhantoms = ReplMonGroup.ReplCurrentNumUnrecoveredPhantomBlobs() +
                ReplMonGroup.ReplNumUnrecoveredPhantomBlobs();
            bool unreplicatedNonPhantoms = ReplMonGroup.ReplCurrentNumUnrecoveredNonPhantomBlobs() +
                ReplMonGroup.ReplNumUnrecoveredNonPhantomBlobs();
            // unsynced VDisks
            ui64 unsyncedVDisks = SyncerMonGroup.SyncerUnsyncedDisks();
            // calculate cumulative status of Skeleton Front overload
            auto light = NKikimrWhiteboard::EFlag::Green;
            for (auto queue : {IntQueueAsyncGets.get(), IntQueueFastGets.get(), IntQueueDiscover.get(),
                               IntQueueLowGets.get(), IntQueueLogPuts.get(), IntQueueHugePutsForeground.get(),
                               IntQueueHugePutsBackground.get()}) {
                light = Max(light, queue->GetCumulativeLight());
            }
            // send a message to Whiteboard
            ctx.Send(SelfId(), new NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate(state, outOfSpaceFlags,
                replicated, unreplicatedPhantoms, unreplicatedNonPhantoms, unsyncedVDisks, light, HasUnreadableBlobs));
            // repeat later
            if (schedule) {
                ctx.Schedule(Config->WhiteboardUpdateInterval, new TEvTimeToUpdateWhiteboard);
            }
        }

        template <class TEventPtr>
        inline void CheckEvent(TEventPtr &ev, const char *msgName) {
            Y_VERIFY_DEBUG(CostModel);
            Y_VERIFY(ev->Get(), "Incoming message of type %s is null at the VDisk border. This MUST never happens, "
                   "check VDisk clients: bufSize# %u", msgName, unsigned(ev->GetSize()));
        }

        template <class TEventPtr>
        void DatabaseAccessDeniedHandle(TEventPtr &ev, const TActorContext &ctx) {
            LOG_ERROR_S(ctx, NKikimrServices::BS_SKELETON, VCtx->VDiskLogPrefix
                    << "Access denied Type# " << Sprintf("0x%08" PRIx32, ev->GetTypeRewrite())
                    << " Sender# " << ev->Sender.ToString()
                    << " OriginScopeId# " << ScopeIdToString(ev->OriginScopeId)
                    << " LocalScopeId# " << ScopeIdToString(AppData(ctx)->LocalScopeId.GetInterconnectScopeId())
                    << " Marker# BSVSF01");
            ++*AccessDeniedMessages;
            TInstant now = TAppData::TimeProvider->Now();
            FillInCostSettingsAndTimestampIfApplicable(ev->Get()->Record, now);
            Reply(ev, ctx, NKikimrProto::ERROR, "access denied", now);
        }

        template <class TEventPtr>
        void DatabaseErrorHandle(TEventPtr &ev, const TActorContext &ctx) {
            SetReceivedTime(ev);
            TInstant now = TAppData::TimeProvider->Now();
            FillInCostSettingsAndTimestampIfApplicable(ev->Get()->Record, now);
            Reply(ev, ctx, NKikimrProto::VDISK_ERROR_STATE, "VDisk is in error state", now);
            // NOTE: VDisk is in StateDatabaseError state, it means recovery failed.
            //       VDisk returns VDISK_ERROR_STATE status to all requests (outside).
        }

        template <class TEventPtr>
        void DatabaseNotReadyHandle(TEventPtr &ev, const TActorContext &ctx) {
            SetReceivedTime(ev);
            TInstant now = TAppData::TimeProvider->Now();
            NotifyIfNotReady(ev, ctx);
            FillInCostSettingsAndTimestampIfApplicable(ev->Get()->Record, now);
            Reply(ev, ctx, NKikimrProto::NOTREADY, "VDisk is not ready", now);
            // NOTE: when database is not ready, we reply with NOTREADY and we do not
            //       pass this message to the Backpressure management subsystem
        }

        void DatabaseErrorHandle(TEvBlobStorage::TEvMonStreamQuery::TPtr& ev, const TActorContext& ctx) {
            ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes("ERROR"));
        }

        void DatabaseNotReadyHandle(TEvBlobStorage::TEvMonStreamQuery::TPtr& ev, const TActorContext& ctx) {
            ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes("NOT_READY"));
        }

        ////////////////////////////////////////////////////////////////////////////
        // HANDLE SECTOR
        ////////////////////////////////////////////////////////////////////////////
        static TInstant CalcDeadline(ui32 val) {
            return val ? TInstant::Seconds(val) : TInstant::Max();
        }

        template <typename TRecord, typename Dimmy = void>
        struct THasMsgQoS {
            static constexpr bool value = false;
        };

        template <typename TRecord>
        struct THasMsgQoS<TRecord, TRecord> {
            static constexpr bool value = true;

            static auto Checking(TRecord &r) {
                return r.MutableMsgQoS();
            }
        };

        template<typename TRecord>
        void FillInCostSettingsAndTimestampIfApplicable(TRecord& record, TInstant now) const
        {
            if constexpr (THasMsgQoS<TRecord>::value) {
                FillInCostSettingsAndTimestampIfRequired(record.MutableMsgQoS(), now);
            }
        }

        void FillInCostSettingsAndTimestampIfRequired(NKikimrBlobStorage::TMsgQoS *qos, TInstant now) const {
            qos->MutableExecTimeStats()->SetReceivedTimestamp(now.GetValue());
            if (qos->GetSendMeCostSettings() && CostModel) {
                CostModel->FillInSettings(*qos->MutableCostSettings());
            }
        }

        template <class TEventPtr>
        void HandleRequestWithQoS(const TActorContext &ctx, TEventPtr &ev, const char *msgName, ui64 cost,
                                  TIntQueueClass &intQueue) {
            CheckEvent(ev, msgName);
            const ui32 recByteSize = ev->Get()->GetCachedByteSize();
            auto &record = ev->Get()->Record;
            auto &msgQoS = *record.MutableMsgQoS();

            // set up reception time
            TInstant now = TAppData::TimeProvider->Now();

            const NBackpressure::TMessageId msgId(msgQoS.GetMsgId());
            const TInstant deadline = CalcDeadline(msgQoS.GetDeadlineSeconds());
            auto extQueueId = msgQoS.GetExtQueueId();
            auto intQueueId = intQueue.IntQueueId;
            msgQoS.SetCost(cost);
            msgQoS.SetIntQueueId(intQueueId);
            ActorIdToProto(ev->Sender, msgQoS.MutableSenderActorId());
            FillInCostSettingsAndTimestampIfRequired(&msgQoS, now);

            // check queue compatibility: it's a contract between BlobStorage Proxy and VDisk,
            // we don't work if queues are incompatible

            bool compatible = Compatible(extQueueId, intQueueId);
            Y_VERIFY(compatible, "%s: %s: extQueue is incompatible with intQueue; intQueue# %s extQueue# %s",
                   VCtx->VDiskLogPrefix.data(), msgName, NKikimrBlobStorage::EVDiskInternalQueueId_Name(intQueueId).data(),
                   NKikimrBlobStorage::EVDiskQueueId_Name(extQueueId).data());

            TExtQueueClass &extQueue = GetExtQueue(extQueueId);
            NBackpressure::TQueueClientId clientId(msgQoS);
            std::unique_ptr<IEventHandle> event = extQueue.Enqueue(ctx, std::unique_ptr<IEventHandle>(
                ev->Forward(SkeletonId).Release()), msgId, cost, *this, clientId);
            if (event) {
                // good, enqueue it in intQueue
                intQueue.Enqueue(ctx, recByteSize, std::move(event), msgId, cost, deadline, extQueueId, *this, clientId);
            }
        }

        bool Compatible(NKikimrBlobStorage::EVDiskQueueId extId, NKikimrBlobStorage::EVDiskInternalQueueId intId) {
            // Abbreviations
            // IU = IntUnknown
            // IAG = IntAsyncGet
            // IFG = IntFastGet
            // IPL = IntPutLog
            // IPHF = IntPutHugeForeground
            // IPHB = IntPutHugeBackground
            // ID  = IntDiscover
            // IL  = IntLowRead
            static bool compatibilityMatrix[8][8] = {
                //                IU     IAG    IFG    IPL    IPHF    IPHB   ID     IL
                /*Unknown*/      {false, false, false, false, false,  false, false, false},
                /*PutTabletLog*/ {false, false, false, true,  true,   false, false, false},
                /*PutAsyncBlob*/ {false, false, false, true,  false,  true , false, false},
                /*PutUserData*/  {false, false, false, true,  true,   false, false, false},
                /*GetAsyncRead*/ {false, true,  false, false, false,  false, false, false},
                /*GetFastRead*/  {false, false, true,  false, false,  false, false, false},
                /*GetDiscover*/  {false, false, false, false, false,  false, true , false},
                /*GetLowRead*/   {false, false, false, false, false,  false, false, true}
            };

            Y_VERIFY_DEBUG(int(extId) >= 0 && int(extId) <= 7 && int(intId) >= 0 && int(intId) <= 7);
            return compatibilityMatrix[extId][intId];
        }

        template <typename TEvPtr>
        void HandlePatchEvent(TEvPtr &ev) {
            const ui64 cost = CostModel->GetCost(*ev->Get());

            auto &record = ev->Get()->Record;

            TLogoBlobID blob;
            TLogoBlobID patchedBlob;
            constexpr bool blobIdWithoutPartId = std::is_same_v<TEvPtr, TEvBlobStorage::TEvVMovedPatch::TPtr>
                    || std::is_same_v<TEvPtr, TEvBlobStorage::TEvVPatchStart::TPtr>;
            if constexpr (blobIdWithoutPartId) {
                blob = LogoBlobIDFromLogoBlobID(record.GetOriginalBlobId());
                patchedBlob = LogoBlobIDFromLogoBlobID(record.GetPatchedBlobId());
            } else {
                blob = LogoBlobIDFromLogoBlobID(record.GetOriginalPartBlobId());
                patchedBlob = LogoBlobIDFromLogoBlobID(record.GetPatchedPartBlobId());
            }

            const char* name = "Unknown";
            TIntQueueClass *queue = IntQueueHugePutsBackground.get();
            if constexpr (std::is_same_v<TEvPtr, TEvBlobStorage::TEvVMovedPatch::TPtr>) {
                LWTRACK(VDiskSkeletonFrontVMovedPatchRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                    VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), blob.TabletID(), blob.BlobSize());
                name = "TEvVMovedPatch";
            } else if constexpr (std::is_same_v<TEvPtr, TEvBlobStorage::TEvVPatchStart::TPtr>) {
                LWTRACK(VDiskSkeletonFrontVPatchStartRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                    VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), blob.TabletID(), blob.BlobSize());
                queue = IntQueueFastGets.get();
                name = "TEvVPatchStart";
            } else if constexpr (std::is_same_v<TEvPtr, TEvBlobStorage::TEvVPatchDiff::TPtr>) {
                LWTRACK(VDiskSkeletonFrontVPatchDiffRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                    VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), blob.TabletID(), blob.BlobSize());
                name = "TEvVPatchDiff";
            } else if constexpr (std::is_same_v<TEvPtr, TEvBlobStorage::TEvVPatchXorDiff::TPtr>) {
                LWTRACK(VDiskSkeletonFrontVPatchXorDiffRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                    VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), blob.TabletID(), blob.BlobSize());
                name = "TEvVPatchXorDiff";
            }

            LOG_DEBUG_S(TActivationContext::AsActorContext(), NKikimrServices::BS_SKELETON, VCtx->VDiskLogPrefix
                    << name << ": received;" << " OriginalBlobId# " << blob << " PatchedBlobId# " << patchedBlob);
            HandleRequestWithQoS(TActivationContext::AsActorContext(), ev, name, cost, *queue);
        }

        void Handle(TEvBlobStorage::TEvVPut::TPtr &ev, const TActorContext &ctx) {
            bool logPutInternalQueue = true;
            const ui64 cost = CostModel->GetCost(*ev->Get(), &logPutInternalQueue);

            const NKikimrBlobStorage::TEvVPut &record = ev->Get()->Record;
            const TLogoBlobID blob = LogoBlobIDFromLogoBlobID(record.GetBlobID());
            LWTRACK(VDiskSkeletonFrontVPutRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                   VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), blob.TabletID(), blob.BlobSize());

            if (logPutInternalQueue) {
                HandleRequestWithQoS(ctx, ev, "TEvVPut", cost, *IntQueueLogPuts);
            } else {
                auto handleClass = ev->Get()->Record.GetHandleClass();
                switch (handleClass) {
                    case NKikimrBlobStorage::TabletLog:
                    case NKikimrBlobStorage::UserData:
                        HandleRequestWithQoS(ctx, ev, "TEvVPut", cost, *IntQueueHugePutsForeground);
                        break;
                    case NKikimrBlobStorage::AsyncBlob:
                        HandleRequestWithQoS(ctx, ev, "TEvVPut", cost, *IntQueueHugePutsBackground);
                        break;
                    default:
                        Y_FAIL("Unexpected case");
                }
            }
        }

        void Handle(TEvBlobStorage::TEvVMultiPut::TPtr &ev, const TActorContext &ctx) {
            bool logPutInternalQueue = true;
            const ui64 cost = CostModel->GetCost(*ev->Get(), &logPutInternalQueue);

            const NKikimrBlobStorage::TEvVMultiPut &record = ev->Get()->Record;
            LWTRACK(VDiskSkeletonFrontVMultiPutRecieved, ev->Get()->Orbit, VCtx->NodeId, VCtx->GroupId,
                 VCtx->Top->GetFailDomainOrderNumber(VCtx->ShortSelfVDisk), record.ItemsSize(),
                 ev->Get()->GetSumBlobSize());

            if (logPutInternalQueue) {
                HandleRequestWithQoS(ctx, ev, "TEvVMultiPut", cost, *IntQueueLogPuts);
            } else {
                auto handleClass = ev->Get()->Record.GetHandleClass();
                switch (handleClass) {
                    case NKikimrBlobStorage::TabletLog:
                    case NKikimrBlobStorage::UserData:
                        HandleRequestWithQoS(ctx, ev, "TEvVMultiPut", cost, *IntQueueHugePutsForeground);
                        break;
                    case NKikimrBlobStorage::AsyncBlob:
                        HandleRequestWithQoS(ctx, ev, "TEvVMultiPut", cost, *IntQueueHugePutsBackground);
                        break;
                    default:
                        Y_FAIL("Unexpected case");
                }
            }
        }

        void Handle(TEvBlobStorage::TEvVGet::TPtr &ev, const TActorContext &ctx) {
            const ui64 cost = CostModel->GetCost(*ev->Get());
            // select correct internal queue
            Y_VERIFY(ev->Get()->Record.HasHandleClass());
            auto cls = ev->Get()->Record.GetHandleClass();
            NKikimrBlobStorage::EVDiskInternalQueueId intQueueId;
            switch (cls) {
                case NKikimrBlobStorage::EGetHandleClass::AsyncRead:
                    intQueueId = NKikimrBlobStorage::EVDiskInternalQueueId::IntGetAsync;
                    break;
                case NKikimrBlobStorage::EGetHandleClass::FastRead:
                    intQueueId = NKikimrBlobStorage::EVDiskInternalQueueId::IntGetFast;
                    break;
                case NKikimrBlobStorage::EGetHandleClass::Discover:
                    intQueueId = NKikimrBlobStorage::EVDiskInternalQueueId::IntGetDiscover;
                    break;
                case NKikimrBlobStorage::EGetHandleClass::LowRead:
                    intQueueId = NKikimrBlobStorage::EVDiskInternalQueueId::IntLowRead;
                    break;
                default:
                    Y_FAIL("Unexpected case");
            }
            TIntQueueClass &queue = GetIntQueue(intQueueId);
            HandleRequestWithQoS(ctx, ev, "TEvVGet", cost, queue);
        }

        void Handle(TEvBlobStorage::TEvVBlock::TPtr &ev, const TActorContext &ctx) {
            const ui64 cost = CostModel->GetCost(*ev->Get());
            HandleRequestWithQoS(ctx, ev, "TEvVBlock", cost, *IntQueueLogPuts);
        }

        void Handle(TEvBlobStorage::TEvVGetBlock::TPtr &ev, const TActorContext &ctx) {
            const ui64 cost = CostModel->GetCost(*ev->Get());
            HandleRequestWithQoS(ctx, ev, "TEvVGetBlock", cost, *IntQueueFastGets);
        }

        void Handle(TEvBlobStorage::TEvVCollectGarbage::TPtr &ev, const TActorContext &ctx) {
            const ui64 cost = CostModel->GetCost(*ev->Get());
            HandleRequestWithQoS(ctx, ev, "TEvVCollectGarbage", cost, *IntQueueLogPuts);
        }

        void Handle(TEvBlobStorage::TEvVGetBarrier::TPtr &ev, const TActorContext &ctx) {
            const ui64 cost = CostModel->GetCost(*ev->Get());
            HandleRequestWithQoS(ctx, ev, "TEvVGetBarrier", cost, *IntQueueFastGets);
        }

        template<typename TEventPtr>
        void HandleRequestWithoutQoS(TEventPtr& ev, const TActorContext& ctx) {
            // we just forward this message to skeleton
            ctx.Send(ev->Forward(SkeletonId));
        }

        void HandleCheckReadiness(TEvBlobStorage::TEvVCheckReadiness::TPtr& ev, const TActorContext& ctx) {
            if (GInfo->CheckScope(TKikimrScopeId(ev->OriginScopeId), ctx, true)) {
                const auto& record = ev->Get()->Record;
                if (record.HasVDiskID()) {
                    const TVDiskID& vdiskId = VDiskIDFromVDiskID(record.GetVDiskID());
                    Y_VERIFY(vdiskId.GroupID == SelfVDiskId.GroupID);
                    Y_VERIFY(TVDiskIdShort(vdiskId) == TVDiskIdShort(SelfVDiskId));
                    if (vdiskId != SelfVDiskId) {
                        if (SelfVDiskId.GroupGeneration < vdiskId.GroupGeneration && record.HasRecentGroup()) {
                            auto newInfo = TBlobStorageGroupInfo::Parse(record.GetRecentGroup(), nullptr, nullptr);
                            ChangeGeneration(vdiskId, newInfo, ctx);
                            Y_VERIFY(vdiskId == SelfVDiskId);
                            const ui32 groupId = newInfo->GroupID;
                            const ui32 generation = newInfo->GroupGeneration;
                            auto ev = std::make_unique<TEvBlobStorage::TEvUpdateGroupInfo>(groupId, generation, *newInfo->Group);
                            Send(MakeBlobStorageNodeWardenID(SelfId().NodeId()), ev.release());
                        } else {
                            return Reply(ev, ctx, NKikimrProto::RACE, "group generation mismatch", TAppData::TimeProvider->Now());
                        }
                    }
                }
                std::optional<NBackpressure::TMessageId> expectedMsgId;
                if (record.HasQoS()) {
                    const auto& qos = record.GetQoS();
                    Y_VERIFY_DEBUG(qos.HasExtQueueId());
                    if (qos.HasExtQueueId()) {
                        auto& queue = GetExtQueue(qos.GetExtQueueId());
                        expectedMsgId = queue.GetExpectedMsgId(ev->Sender);
                    }
                }
                Reply(ev, ctx, NKikimrProto::OK, TString(), TAppData::TimeProvider->Now(), expectedMsgId);
            } else {
                Reply(ev, ctx, NKikimrProto::ERROR, "access denied", TAppData::TimeProvider->Now());
            }
        }


        ////////////////////////////////////////////////////////////////////////////
        // Set receiving time
        ////////////////////////////////////////////////////////////////////////////
        template <typename TPtr>
        void SetReceivedTime(TPtr& ev) {
            using TRecord = decltype(ev->Get()->Record);
            if constexpr (std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVMovedPatch>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVPatchStart>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVPatchDiff>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVPatchXorDiff>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVPut>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVMultiPut>
                       || std::is_convertible_v<TRecord, NKikimrBlobStorage::TEvVGet>)
            {
                const double usPerCycle = 1000000.0 / NHPTimer::GetCyclesPerSecond();
                ev->Get()->Record.MutableTimestamps()->SetReceivedByVDiskUs(GetCycleCountFast() * usPerCycle);
            }
        }

        ////////////////////////////////////////////////////////////////////////////
        // REPLY SECTOR
        ////////////////////////////////////////////////////////////////////////////
        template<typename TPtr>
        void Reply(TPtr& ev, const TActorContext& ctx, NKikimrProto::EReplyStatus status, const TString& errorReason,
                TInstant now, const TWindowStatus& wstatus) {
            wstatus.Serialize(*ev->Get()->Record.MutableMsgQoS()->MutableWindow());
            Reply(ev, ctx, status, errorReason, now);
        }

        template<typename TPtr>
        void Reply(TPtr& ev, const TActorContext& ctx, NKikimrProto::EReplyStatus status, const TString& errorReason,
                TInstant now) {
            using namespace NErrBuilder;
            auto res = ErroneousResult(VCtx, status, errorReason, ev, now, nullptr, SelfVDiskId, VDiskIncarnationGuid, GInfo);
            SendVDiskResponse(ctx, ev->Sender, res.release(), ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVCheckReadiness::TPtr &ev, const TActorContext &ctx,
                NKikimrProto::EReplyStatus status, const TString& /*errorReason*/, TInstant /*now*/,
                const std::optional<NBackpressure::TMessageId>& expectedMsgId = std::nullopt) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            auto res = std::make_unique<TEvBlobStorage::TEvVCheckReadinessResult>(status);
            auto& record = res->Record;
            SetRacingGroupInfo(ev->Get()->Record, record, GInfo);
            if (expectedMsgId) {
                expectedMsgId->Serialize(*record.MutableExpectedMsgId());
            }
            if (CostModel && status == NKikimrProto::OK) {
                CostModel->FillInSettings(*record.MutableCostSettings());
            }
            ctx.Send(ev->Sender, res.release(), flags, ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVCompact::TPtr &ev, const TActorContext &ctx,
                NKikimrProto::EReplyStatus status, const TString& /*errorReason*/, TInstant /*now*/) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            ctx.Send(ev->Sender, new TEvBlobStorage::TEvVCompactResult(status, ev->Get()->Record.GetVDiskID()),
                flags, ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVDefrag::TPtr &ev, const TActorContext &ctx,
                NKikimrProto::EReplyStatus status, const TString& /*errorReason*/, TInstant /*now*/) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            ctx.Send(ev->Sender, new TEvBlobStorage::TEvVDefragResult(status, ev->Get()->Record.GetVDiskID()),
                flags, ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVBaldSyncLog::TPtr &ev, const TActorContext &ctx,
                NKikimrProto::EReplyStatus status, const TString& /*errorReason*/, TInstant /*now*/) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            ctx.Send(ev->Sender, new TEvBlobStorage::TEvVBaldSyncLogResult(status, ev->Get()->Record.GetVDiskID()),
                flags, ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVStatus::TPtr &ev, const TActorContext &ctx,
                NKikimrProto::EReplyStatus status, const TString& /*errorReason*/, TInstant /*now*/) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            ctx.Send(ev->Sender, new TEvBlobStorage::TEvVStatusResult(status, ev->Get()->Record.GetVDiskID()),
                flags, ev->Cookie);
        }

        void Reply(TEvBlobStorage::TEvVAssimilate::TPtr& ev, const TActorContext& ctx,
                NKikimrProto::EReplyStatus status, const TString& errorReason, TInstant /*now*/) {
            const ui32 flags = IEventHandle::MakeFlags(ev->GetChannel(), 0);
            ctx.Send(ev->Sender, new TEvBlobStorage::TEvVAssimilateResult(status, errorReason), flags, ev->Cookie);
        }
        // FIXME: don't forget about counters


        ////////////////////////////////////////////////////////////////////////////
        // OTHER SECTOR
        ////////////////////////////////////////////////////////////////////////////
        void Handle(NMon::TEvHttpInfo::TPtr &ev, const TActorContext &ctx) {
            // we process mon requests out of order
            const TCgiParameters& cgi = ev->Get()->Request.GetParams();
            const TString& type = cgi.Get("type");
            TString html = (type == TString()) ? GenerateHtmlState(ctx) : TString();

            auto aid = ctx.Register(CreateFrontSkeletonMonRequestHandler(SelfVDiskId, ctx.SelfID, SkeletonId,
                ctx.SelfID, Config, Top, ev, html));
            ActiveActors.Insert(aid);
        }

        void Handle(TEvVDiskStatRequest::TPtr &ev) {
            ev->Rewrite(ev->GetTypeRewrite(), SkeletonId);
            TActivationContext::Send(ev.Release());
        }

        void Handle(TEvGetLogoBlobRequest::TPtr &ev) {
            auto aid = Register(CreateFrontSkeletonGetLogoBlobRequestHandler(SelfVDiskId, SelfId(), SkeletonId,
                Config, Top, ev));
            ActiveActors.Insert(aid);
        }


        void Handle(TEvVDiskRequestCompleted::TPtr &ev, const TActorContext &ctx) {
            const TVMsgContext &msgCtx = ev->Get()->Ctx;
            std::unique_ptr<IEventHandle> event = std::move(ev->Get()->Event);
            TExtQueueClass &extQueue = GetExtQueue(msgCtx.ExtQueueId);
            extQueue.Completed(ctx, msgCtx, event->GetBase());
            TIntQueueClass &intQueue = GetIntQueue(msgCtx.IntQueueId);
            intQueue.Completed(ctx, msgCtx, *this);
            TActivationContext::Send(event.release());
        }

        void ChangeGeneration(const TVDiskID& vdiskId, const TIntrusivePtr<TBlobStorageGroupInfo>& info,
                const TActorContext& ctx) {
            // check group id
            Y_VERIFY(info->GroupID == GInfo->GroupID, "GroupId# %" PRIu32 " new GroupId# %" PRIu32,
                GInfo->GroupID, info->GroupID);

            // check target disk id
            Y_VERIFY(TVDiskIdShort(SelfVDiskId) == TVDiskIdShort(vdiskId), "Incorrect target VDiskId"
                    " SelfVDiskId# %s new VDiskId# %s", SelfVDiskId.ToString().data(), vdiskId.ToString().data());

            // check the disk location inside group
            Y_VERIFY(info->GetVDiskId(TVDiskIdShort(vdiskId)) == vdiskId);

            // check that NewInfo has the same topology as the one VDisk started with
            Y_VERIFY(VCtx->Top->EqualityCheck(info->GetTopology()));

            // check generation for possible race
            if (info->GroupGeneration <= GInfo->GroupGeneration) {
                // its just a race -- we already have newer group generation
                return;
            }

            // all checks passed
            LOG_INFO_S(ctx, BS_SKELETON, VCtx->VDiskLogPrefix << "VDisk Generation Change success;"
                    << " new VDiskId# " << vdiskId
                    << " Marker# BSVSF02");

            // update GroupInfo-related fields
            GInfo = info;
            const auto& prevVDiskId = std::exchange(SelfVDiskId, vdiskId);

            // forward message to Skeleton
            ctx.Send(SkeletonId, new TEvVGenerationChange(vdiskId, info));

            // notify whiteboard
            Send(NNodeWhiteboard::MakeNodeWhiteboardServiceId(SelfId().NodeId()),
                new NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateGenerationChange(prevVDiskId,
                SelfVDiskId.GroupGeneration, Config->BaseInfo.WhiteboardInstanceGuid));
        }

        void Handle(TEvVGenerationChange::TPtr &ev, const TActorContext &ctx) {
            auto *msg = ev->Get();
            ChangeGeneration(msg->NewVDiskId, msg->NewInfo, ctx);
        }

        void Handle(TEvPDiskErrorStateChange::TPtr &ev, const TActorContext &ctx) {
            LOG_ERROR_S(ctx, NKikimrServices::BS_SKELETON, VCtx->VDiskLogPrefix
                    << "SkeletonFront: got TEvPDiskErrorStateChange;"
                    << " state# " << TPDiskErrorState::StateToString(ev->Get()->State)
                    << " Marker# BSVSF03");


            // switch skeleton state to PDiskError
            SkeletonFrontGroup->ResetCounters();
            VDiskMonGroup.VDiskState(NKikimrWhiteboard::EVDiskState::PDiskError);
            // send poison pill to Skeleton to shutdown it
            ctx.Send(SkeletonId, new TEvents::TEvPoisonPill());
            SkeletonId = {};
            // go to error state
            Become(&TThis::StateDatabaseError);
            // update status in NodeWarden
            const TActorId& warden = MakeBlobStorageNodeWardenID(SelfId().NodeId());
            const auto& base = Config->BaseInfo;
            ctx.Send(warden, new TEvStatusUpdate(ctx.SelfID.NodeId(), base.PDiskId, base.VDiskSlotId, NKikimrBlobStorage::EVDiskStatus::ERROR));
            // drop messages in internal queues
            for (auto *q : {&IntQueueAsyncGets, &IntQueueFastGets, &IntQueueDiscover, &IntQueueLowGets, &IntQueueLogPuts,
                    &IntQueueHugePutsForeground, &IntQueueHugePutsBackground}) {
                (*q)->DropWithError(ctx, *this);
            }
            // drop external queues
            DisconnectClients(ctx);
        }

        void DisconnectClients(const TActorContext& ctx) {
            const auto& base = Config->BaseInfo;
            const TActorId& serviceId = MakeBlobStorageVDiskID(SelfId().NodeId(), base.PDiskId, base.VDiskSlotId);
            for (auto *q : {&ExtQueueAsyncGets, &ExtQueueFastGets, &ExtQueueDiscoverGets, &ExtQueueLowGets,
                    &ExtQueueTabletLogPuts, &ExtQueueAsyncBlobPuts, &ExtQueueUserDataPuts}) {
                q->DisconnectClients(ctx, serviceId);
            }
        }

    public:
        TExtQueueClass &GetExtQueue(NKikimrBlobStorage::EVDiskQueueId extQueueId) {
            TExtQueueClass *extQueue = nullptr;
            switch (extQueueId) {
                case NKikimrBlobStorage::EVDiskQueueId::PutTabletLog:
                    extQueue = &ExtQueueTabletLogPuts;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::PutAsyncBlob:
                    extQueue = &ExtQueueAsyncBlobPuts;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::PutUserData:
                    extQueue = &ExtQueueUserDataPuts;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::GetAsyncRead:
                    extQueue = &ExtQueueAsyncGets;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::GetFastRead:
                    extQueue = &ExtQueueFastGets;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::GetDiscover:
                    extQueue = &ExtQueueDiscoverGets;
                    break;
                case NKikimrBlobStorage::EVDiskQueueId::GetLowRead:
                    extQueue = &ExtQueueLowGets;
                    break;
                default: Y_FAIL("Unexpected case extQueueId# %" PRIu32, static_cast<ui32>(extQueueId));
            }
            return *extQueue;
        }

        TIntQueueClass &GetIntQueue(NKikimrBlobStorage::EVDiskInternalQueueId intQueueId) {
            TIntQueueClass *intQueue = nullptr;
            switch (intQueueId) {
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntGetAsync:
                    intQueue = IntQueueAsyncGets.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntGetFast:
                    intQueue = IntQueueFastGets.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntGetDiscover:
                    intQueue = IntQueueDiscover.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntLowRead:
                    intQueue = IntQueueLowGets.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntPutLog:
                    intQueue = IntQueueLogPuts.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntPutHugeForeground:
                    intQueue = IntQueueHugePutsForeground.get();
                    break;
                case NKikimrBlobStorage::EVDiskInternalQueueId::IntPutHugeBackground:
                    intQueue = IntQueueHugePutsBackground.get();
                    break;
                default: Y_FAIL("Unexpected case");
            }
            return *intQueue;
        }

    private:
        void Die(const TActorContext &ctx) override {
            ActiveActors.KillAndClear(ctx);
            TActorBootstrapped::Die(ctx);
        }

        void Handle(TEvents::TEvActorDied::TPtr &ev, const TActorContext &ctx) {
            Y_UNUSED(ctx);
            ActiveActors.Erase(ev->Sender);
        }

        void HandleCommenceRepl(const TActorContext& /*ctx*/) {
            TActivationContext::Send(new IEventHandle(TEvBlobStorage::EvCommenceRepl, 0, SkeletonId, SelfId(), nullptr, 0));
        }

        void Handle(TEvReportScrubStatus::TPtr ev, const TActorContext& ctx) {
            HasUnreadableBlobs = ev->Get()->HasUnreadableBlobs;
            UpdateWhiteboard(ctx, false);
        }

        void Handle(NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate::TPtr ev, const TActorContext& ctx) {
            auto& record = ev->Get()->Record;
            VDiskIDFromVDiskID(SelfVDiskId, record.MutableVDiskId());
            record.SetInstanceGuid(Config->BaseInfo.WhiteboardInstanceGuid);
            ctx.Send(ev->Forward(NNodeWhiteboard::MakeNodeWhiteboardServiceId(SelfId().NodeId())));
        }

        // while local db recovery is in progress, we use this state function to handle requests
        STRICT_STFUNC(StateLocalRecoveryInProgress,
            HFunc(TEvBlobStorage::TEvVMovedPatch, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchStart, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchDiff, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchXorDiff, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPut, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVMultiPut, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGet, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVBlock, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGetBlock, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVCollectGarbage, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGetBarrier, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVStatus, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVAssimilate, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVDbStat, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvMonStreamQuery, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVSync, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVSyncFull, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVSyncGuid, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVCheckReadiness, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVCompact, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVDefrag, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVBaldSyncLog, DatabaseNotReadyHandle)
            HFunc(TEvVGenerationChange, Handle)
            HFunc(TEvPDiskErrorStateChange, Handle)
            HFunc(NMon::TEvHttpInfo, Handle)
            hFunc(TEvVDiskStatRequest, Handle)
            hFunc(TEvGetLogoBlobRequest, Handle)
            HFunc(TEvFrontRecoveryStatus, Handle)
            HFunc(TEvVDiskRequestCompleted, Handle)
            CFunc(TEvBlobStorage::EvTimeToUpdateWhiteboard, UpdateWhiteboard)
            CFunc(NActors::TEvents::TSystem::PoisonPill, Die)
            HFunc(TEvents::TEvActorDied, Handle)
            CFunc(TEvBlobStorage::EvCommenceRepl, HandleCommenceRepl)
            FFunc(TEvBlobStorage::EvScrubAwait, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCaptureVDiskLayout, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCompactVDisk, ForwardToSkeleton)
            HFunc(NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate, Handle)
            FFunc(TEvBlobStorage::EvForwardToSkeleton, HandleForwardToSkeleton)
        )

        // while recovering sync guid we use this state function to handle requests
        STRICT_STFUNC(StateSyncGuidRecoveryInProgress,
            HFunc(TEvBlobStorage::TEvVMovedPatch, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchStart, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchDiff, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPatchXorDiff, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVPut, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVMultiPut, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGet, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVBlock, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGetBlock, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVCollectGarbage, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVGetBarrier, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVStatus, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVAssimilate, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVDbStat, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvMonStreamQuery, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVSync, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVSyncFull, DatabaseNotReadyHandle)
            // NOTE: Below is the only diff from StateLocalRecoveryInProgress --
            //       we handle TEvBlobStorage::TEvVSyncGuid
            HFunc(TEvBlobStorage::TEvVSyncGuid, HandleRequestWithoutQoS)
            HFunc(TEvBlobStorage::TEvVCheckReadiness, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVCompact, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVDefrag, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVBaldSyncLog, DatabaseNotReadyHandle)
            HFunc(TEvVGenerationChange, Handle)
            HFunc(TEvPDiskErrorStateChange, Handle)
            HFunc(NMon::TEvHttpInfo, Handle)
            hFunc(TEvVDiskStatRequest, Handle)
            hFunc(TEvGetLogoBlobRequest, Handle)
            HFunc(TEvFrontRecoveryStatus, Handle)
            HFunc(TEvVDiskRequestCompleted, Handle)
            CFunc(TEvBlobStorage::EvTimeToUpdateWhiteboard, UpdateWhiteboard)
            CFunc(NActors::TEvents::TSystem::PoisonPill, Die)
            HFunc(TEvents::TEvActorDied, Handle)
            CFunc(TEvBlobStorage::EvCommenceRepl, HandleCommenceRepl)
            FFunc(TEvBlobStorage::EvControllerScrubStartQuantum, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvScrubAwait, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCaptureVDiskLayout, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCompactVDisk, ForwardToSkeleton)
            HFunc(TEvReportScrubStatus, Handle)
            HFunc(NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate, Handle)
            FFunc(TEvBlobStorage::EvForwardToSkeleton, HandleForwardToSkeleton)
        )

        STRICT_STFUNC(StateDatabaseError,
            HFunc(TEvBlobStorage::TEvVMovedPatch, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVPatchStart, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVPatchDiff, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVPatchXorDiff, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVPut, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVMultiPut, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVGet, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVBlock, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVGetBlock, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVCollectGarbage, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVGetBarrier, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVStatus, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVAssimilate, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVDbStat, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvMonStreamQuery, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVSync, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVSyncFull, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVSyncGuid, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVCheckReadiness, DatabaseErrorHandle)
            HFunc(TEvBlobStorage::TEvVCompact, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVDefrag, DatabaseNotReadyHandle)
            HFunc(TEvBlobStorage::TEvVBaldSyncLog, DatabaseNotReadyHandle)
            HFunc(TEvVGenerationChange, Handle)
            HFunc(TEvPDiskErrorStateChange, Handle)
            HFunc(NMon::TEvHttpInfo, Handle)
            hFunc(TEvVDiskStatRequest, Handle)
            hFunc(TEvGetLogoBlobRequest, Handle)
            CFunc(TEvBlobStorage::EvTimeToUpdateWhiteboard, UpdateWhiteboard)
            CFunc(NActors::TEvents::TSystem::PoisonPill, Die)
            HFunc(TEvents::TEvActorDied, Handle)
            CFunc(TEvBlobStorage::EvCommenceRepl, HandleCommenceRepl)
            FFunc(TEvBlobStorage::EvControllerScrubStartQuantum, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvScrubAwait, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCaptureVDiskLayout, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCompactVDisk, ForwardToSkeleton)
            HFunc(TEvReportScrubStatus, Handle)
            IgnoreFunc(TEvVDiskRequestCompleted)
            HFunc(NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate, Handle)
            FFunc(TEvBlobStorage::EvForwardToSkeleton, HandleForwardToSkeleton)
        )

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Events checking: RACE and access control
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        template <typename TEv>
        static constexpr bool IsPatchEvent = std::is_same_v<TEv, TEvBlobStorage::TEvVMovedPatch>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVPatchStart>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVPatchDiff>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVPatchXorDiff>;

        template <typename TEv>
        static constexpr bool IsWithoutQoS = std::is_same_v<TEv, TEvBlobStorage::TEvVStatus>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVDbStat>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVCompact>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVDefrag>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVBaldSyncLog>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVAssimilate>;

        template <typename TEv>
        static constexpr bool IsValidatable = std::is_same_v<TEv, TEvBlobStorage::TEvVMultiPut>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVGet>
                || std::is_same_v<TEv, TEvBlobStorage::TEvVPut>;

        template<typename TEventType>
        void CheckExecute(TAutoPtr<TEventHandle<TEventType>>& ev, const TActorContext& ctx) {
            if constexpr (IsPatchEvent<TEventType>) {
                HandlePatchEvent(ev);
            } else if constexpr (IsWithoutQoS<TEventType>) {
                HandleRequestWithoutQoS(ev, ctx);
            } else {
                Handle(ev, ctx);
            }
        }

        template <typename TEv>
        bool Validate(TEv* ev, TString& errorReason) const {
            if constexpr (IsValidatable<TEv>) {
                return ev->Validate(errorReason);
            }
            return true;
        }

        template <typename TEventType>
        void Check(TAutoPtr<TEventHandle<TEventType>>& ev, const TActorContext& ctx) {
            const auto& record = ev->Get()->Record;
            if (!SelfVDiskId.SameDisk(record.GetVDiskID())) {
                return Reply(ev, ctx, NKikimrProto::RACE, "group generation mismatch", TAppData::TimeProvider->Now());
            } else if (!GInfo->CheckScope(TKikimrScopeId(ev->OriginScopeId), ctx, true)) {
                DatabaseAccessDeniedHandle(ev, ctx);
            } else {
                SetReceivedTime(ev);
                CheckExecute(ev, ctx);
            }
        }

        template<typename TEv>
        void ValidateEvent(TAutoPtr<TEventHandle<TEv>>& ev, const TActorContext& ctx) {
            TString errorReason;
            bool isQueryValid = Validate(ev->Get(), errorReason);
            if (!isQueryValid) {
                Reply(ev, ctx, NKikimrProto::ERROR, errorReason, TAppData::TimeProvider->Now());
                return;
            }
            Check(ev, ctx);
        };

        void ForwardToSkeleton(STFUNC_SIG) {
            ctx.Send(ev->Forward(SkeletonId));
        }

        void HandleForwardToSkeleton(STFUNC_SIG) {
            ev->Rewrite(ev->Type, SkeletonId);
            ctx.Send(ev);
        }

        STRICT_STFUNC(StateFunc,
            HFunc(TEvBlobStorage::TEvVMovedPatch, Check)
            HFunc(TEvBlobStorage::TEvVPatchStart, Check)
            HFunc(TEvBlobStorage::TEvVPatchDiff, Check)
            HFunc(TEvBlobStorage::TEvVPatchXorDiff, Check)
            HFunc(TEvBlobStorage::TEvVPut, ValidateEvent)
            HFunc(TEvBlobStorage::TEvVMultiPut, ValidateEvent)
            HFunc(TEvBlobStorage::TEvVGet, ValidateEvent)
            HFunc(TEvBlobStorage::TEvVBlock, Check)
            HFunc(TEvBlobStorage::TEvVGetBlock, Check)
            HFunc(TEvBlobStorage::TEvVCollectGarbage, Check)
            HFunc(TEvBlobStorage::TEvVGetBarrier, Check)
            HFunc(TEvBlobStorage::TEvVStatus, Check)
            HFunc(TEvBlobStorage::TEvVAssimilate, Check)
            HFunc(TEvBlobStorage::TEvVDbStat, Check)
            HFunc(TEvBlobStorage::TEvMonStreamQuery, HandleRequestWithoutQoS)
            HFunc(TEvBlobStorage::TEvVSync, HandleRequestWithoutQoS)
            HFunc(TEvBlobStorage::TEvVSyncFull, HandleRequestWithoutQoS)
            HFunc(TEvBlobStorage::TEvVSyncGuid, HandleRequestWithoutQoS)
            HFunc(TEvBlobStorage::TEvVCheckReadiness, HandleCheckReadiness)
            HFunc(TEvBlobStorage::TEvVCompact, Check)
            HFunc(TEvBlobStorage::TEvVDefrag, Check)
            HFunc(TEvBlobStorage::TEvVBaldSyncLog, Check)
            HFunc(TEvVGenerationChange, Handle)
            HFunc(TEvPDiskErrorStateChange, Handle)
            HFunc(NMon::TEvHttpInfo, Handle)
            hFunc(TEvVDiskStatRequest, Handle)
            hFunc(TEvGetLogoBlobRequest, Handle)
            // TEvFrontRecoveryStatus
            HFunc(TEvVDiskRequestCompleted, Handle)
            CFunc(TEvBlobStorage::EvTimeToUpdateWhiteboard, UpdateWhiteboard)
            CFunc(NActors::TEvents::TSystem::PoisonPill, Die)
            HFunc(TEvents::TEvActorDied, Handle)
            CFunc(TEvBlobStorage::EvCommenceRepl, HandleCommenceRepl)
            FFunc(TEvBlobStorage::EvControllerScrubStartQuantum, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvScrubAwait, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCaptureVDiskLayout, ForwardToSkeleton)
            FFunc(TEvBlobStorage::EvCompactVDisk, ForwardToSkeleton)
            HFunc(TEvReportScrubStatus, Handle)
            HFunc(NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateUpdate, Handle)
            FFunc(TEvBlobStorage::EvForwardToSkeleton, HandleForwardToSkeleton)
        )

#define HFuncStatus(TEvType, status, errorReason, now, wstatus) \
    case TEvType::EventType: \
    { \
        TEvType::TPtr x(static_cast<TEventHandle<TEvType>*>(ev.release())); \
        Reply(x, ctx, status, errorReason, now, wstatus); \
        break; \
    }

    public:
        void ReplyFunc(std::unique_ptr<IEventHandle> ev, const ::NActors::TActorContext &ctx,
                       NKikimrProto::EReplyStatus status, const TString& errorReason, TInstant now,
                       const TWindowStatus &wstatus) {
            switch (ev->GetTypeRewrite()) {
                HFuncStatus(TEvBlobStorage::TEvVMovedPatch, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVPatchStart, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVPatchDiff, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVPatchXorDiff, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVPut, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVMultiPut, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVGet, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVBlock, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVGetBlock, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVCollectGarbage, status, errorReason, now, wstatus);
                HFuncStatus(TEvBlobStorage::TEvVGetBarrier, status, errorReason, now, wstatus);
                default: Y_VERIFY_DEBUG(false, "Unsupported message %d", ev->GetTypeRewrite());
            }
        }

    public:
        static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
            return NKikimrServices::TActivity::BS_SKELETON_FRONT;
        }

        static TIntrusivePtr<::NMonitoring::TDynamicCounters> CreateVDiskCounters(
                TIntrusivePtr<TVDiskConfig> cfg,
                TIntrusivePtr<TBlobStorageGroupInfo> info,
                TIntrusivePtr<::NMonitoring::TDynamicCounters> counters) {

            // create 'vdisks' service counters
            auto vdiskCounters = GetServiceCounters(counters, "vdisks");

            // add 'storagePool' label
            vdiskCounters = vdiskCounters->GetSubgroup("storagePool", cfg->BaseInfo.StoragePoolName);

            // add 'group' label
            const ui32 blobstorageGroupId = info->GroupID;
            vdiskCounters = vdiskCounters->GetSubgroup("group", Sprintf("%09" PRIu32, blobstorageGroupId));

            // add 'orderNumber' label (VDisk order number in the group)
            const ui32 vdiskOrderNumber = info->GetOrderNumber(cfg->BaseInfo.VDiskIdShort);
            vdiskCounters = vdiskCounters->GetSubgroup("orderNumber", Sprintf("%02" PRIu32, vdiskOrderNumber));

            // add 'pdisk' label as a local id of pdisk
            const ui32 pdiskId = cfg->BaseInfo.PDiskId;
            vdiskCounters = vdiskCounters->GetSubgroup("pdisk", Sprintf("%09" PRIu32, pdiskId));

            // add 'media'
            const auto media = cfg->BaseInfo.DeviceType;
            vdiskCounters = vdiskCounters->GetSubgroup("media", to_lower(NPDisk::DeviceTypeStr(media, true)));

            return vdiskCounters;
        }

        TSkeletonFront(TIntrusivePtr<TVDiskConfig> cfg, TIntrusivePtr<TBlobStorageGroupInfo> info,
                       const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters)
            : TActorBootstrapped<TSkeletonFront>()
            , VCtx()
            , Config(cfg)
            , GInfo(info)
            , Top(GInfo->PickTopology())
            , SelfVDiskId(GInfo->GetVDiskId(Config->BaseInfo.VDiskIdShort))
            , SkeletonId()
            , VDiskCounters(CreateVDiskCounters(cfg, info, counters))
            , SkeletonFrontGroup(VDiskCounters->GetSubgroup("subsystem", "skeletonfront"))
            , AccessDeniedMessages(SkeletonFrontGroup->GetCounter("AccessDeniedMessages", true))

            , ExtQueueAsyncGets(NKikimrBlobStorage::EVDiskQueueId::GetAsyncRead,
                                "AsyncGet",
                                Config->SkeletonFrontExtGetAsync_TotalCost,
                                Config->SkeletonFrontQueueBackpressureCheckMsgId,
                                SkeletonFrontGroup,
                                cfg)
            , ExtQueueFastGets(NKikimrBlobStorage::EVDiskQueueId::GetFastRead,
                               "FastGet",
                               Config->SkeletonFrontExtGetFast_TotalCost,
                               Config->SkeletonFrontQueueBackpressureCheckMsgId,
                               SkeletonFrontGroup,
                               cfg)
            , ExtQueueDiscoverGets(NKikimrBlobStorage::EVDiskQueueId::GetDiscover,
                               "DiscoverGet",
                               Config->SkeletonFrontExtGetDiscover_TotalCost,
                               Config->SkeletonFrontQueueBackpressureCheckMsgId,
                               SkeletonFrontGroup,
                               cfg)
            , ExtQueueLowGets(NKikimrBlobStorage::EVDiskQueueId::GetLowRead,
                               "LowGet",
                               Config->SkeletonFrontExtGetLow_TotalCost,
                               Config->SkeletonFrontQueueBackpressureCheckMsgId,
                               SkeletonFrontGroup,
                               cfg)
            , ExtQueueTabletLogPuts(NKikimrBlobStorage::EVDiskQueueId::PutTabletLog,
                                    "PutTabletLog",
                                    Config->SkeletonFrontExtPutTabletLog_TotalCost,
                                    Config->SkeletonFrontQueueBackpressureCheckMsgId,
                                    SkeletonFrontGroup,
                                    cfg)
            , ExtQueueAsyncBlobPuts(NKikimrBlobStorage::EVDiskQueueId::PutAsyncBlob,
                                    "PutAsyncBlob",
                                    Config->SkeletonFrontExtPutAsyncBlob_TotalCost,
                                    Config->SkeletonFrontQueueBackpressureCheckMsgId,
                                    SkeletonFrontGroup,
                                    cfg)
            , ExtQueueUserDataPuts(NKikimrBlobStorage::EVDiskQueueId::PutUserData,
                                   "PutUserData",
                                   Config->SkeletonFrontExtPutUserData_TotalCost,
                                   Config->SkeletonFrontQueueBackpressureCheckMsgId,
                                   SkeletonFrontGroup,
                                   cfg)
            , CostModel()
            , ReplMonGroup(VDiskCounters, "subsystem", "repl")
            , SyncerMonGroup(VDiskCounters, "subsystem", "syncer")
            , VDiskMonGroup(VDiskCounters, "subsystem", "state")
        {
            ReplMonGroup.ReplUnreplicatedVDisks() = 1;
            VDiskMonGroup.VDiskState(NKikimrWhiteboard::EVDiskState::Initial);
        }
    };

    ////////////////////////////////////////////////////////////////////////////
    // SKELETON FRONT
    ////////////////////////////////////////////////////////////////////////////
    IActor* CreateVDiskSkeletonFront(const TIntrusivePtr<TVDiskConfig> &cfg,
                                     const TIntrusivePtr<TBlobStorageGroupInfo> &info,
                                     const TIntrusivePtr<::NMonitoring::TDynamicCounters> &counters) {
        return new TSkeletonFront(cfg, info, counters);
    }

} // NKikimr
