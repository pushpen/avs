/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
//
//  FlowManager.h
//  zcall-ios
//


#import <Foundation/Foundation.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
@compatibility_alias UIView NSView;
#endif

#define FlowManagerSelfUserParticipantIdentifier @"self"
#define FlowManagerOtherUserParticipantIdentifier @"other"

#define FlowManagerCreatePreviewNotification @"AVSFlowManagerCreatePreviewNotification"
#define FlowManagerReleasePreviewNotification @"AVSFlowManagerReleasePreviewNotification"
#define FlowManagerCreateViewNotification @"AVSFlowManagerCreateViewNotification"
#define FlowManagerReleaseViewNotification @"AVSFlowManagerReleaseViewNotification"

#import "AVSMediaManager.h"

/* IMPORTANT: Make sure to keep these enums in sync with avs_flowmgr.h */

typedef NS_ENUM(int, AVSFlowManagerCategory) {
    FLOWMANAGER_CATEGORY_NORMAL,
    FLOWMANAGER_CATEGORY_HOLD,
    FLOWMANAGER_CATEGORY_PLAYBACK,
    FLOWMANAGER_CATEGORY_CALL,
    FLOWMANAGER_CATEGORY_CALL_VIDEO,
};

typedef NS_ENUM(int, AVSFlowManagerLogLevel) {
	FLOWMANAGER_LOGLEVEL_DEBUG = 0,
	FLOWMANAGER_LOGLEVEL_INFO  = 1,
	FLOWMANAGER_LOGLEVEL_WARN  = 2,
	FLOWMANAGER_LOGLEVEL_ERROR = 3,
};

typedef NS_ENUM(int, AVSFlowManagerAudioSource) {
	FLOMANAGER_AUDIO_SOURCE_INTMIC,
	FLOMANAGER_AUDIO_SOURCE_EXTMIC,
	FLOMANAGER_AUDIO_SOURCE_HEADSET,
	FLOMANAGER_AUDIO_SOURCE_BT,	
	FLOMANAGER_AUDIO_SOURCE_LINEIN,
	FLOMANAGER_AUDIO_SOURCE_SPDIF	
};

typedef NS_ENUM(int, AVSFlowManagerAudioPlay) {
	FLOWMANAGER_AUDIO_PLAY_EARPIECE,
	FLOWMANAGER_AUDIO_PLAY_SPEAKER,
	FLOWMANAGER_AUDIO_PLAY_HEADSET,
	FLOWMANAGER_AUDIO_PLAY_BT,
	FLOWMANAGER_AUDIO_PLAY_LINEOUT,
	FLOWMANAGER_AUDIO_PLAY_SPDIF
};

typedef NS_ENUM(int, AVSFlowActivityState) {
    AVSFlowActivityStateInvalid = 0, ///< Should never be used
    AVSFlowActivityStateCallActive = 1, ///< Has an active call
    AVSFlowActivityStateNoActivity = 2, ///< Is idle
};

typedef NS_ENUM(int, AVSFlowManagerVideoSendState) {
	FLOWMANAGER_VIDEO_SEND_NONE = 0,
	FLOWMANAGER_VIDEO_PREVIEW   = 1,
	FLOWMANAGER_VIDEO_SEND      = 2
};

@interface AVSVideoCaptureDevice : NSObject
@property (readonly) NSString *deviceId;
@property (readonly) NSString *deviceName;
@end

@protocol AVSFlowManagerDelegate<NSObject>
+ (void)logMessage:(NSString *)msg;
- (BOOL)requestWithPath:(NSString *)path
         method:(NSString *)method
      mediaType:(NSString *)mtype
        content:(NSData *)content
        context:(void const *)ctx;

- (void)didEstablishMediaInConversation:(NSString *)convid;

- (void)setFlowManagerActivityState:(AVSFlowActivityState)activityState;

- (void)networkQuality:(float)q conversation:(NSString *)convid;

- (void)mediaWarningOnConversation:(NSString *)convId; 

- (void)errorHandler:(int)err conversationId:(NSString *)convid context:(void const*)ctx;

- (void)vmStatushandler:(BOOL)is_playing current_time:(int)cur_time_ms length:(int)file_length_ms;
@optional

- (void) didUpdateVolume:(double)volume conversationId:(NSString *)convid participantId:(NSString *)participantId;

- (void)didEstablishMediaInConversation:(NSString *)convid forUser:(NSString *)userid;

- (void)conferenceParticipantsDidChange:(NSArray *)participants
                         inConversation:(NSString *)convId;
@end

struct flowmgr;

@interface AVSFlowManager : NSObject

+ (void)setLogLevel:(AVSFlowManagerLogLevel)logLevel;
+ (NSComparator)conferenceComparator;

- (instancetype)init;
- (instancetype)initWithDelegate:(id<AVSFlowManagerDelegate>)delegate mediaManager:(id)mediaManager;
- (instancetype)initWithDelegate:(id<AVSFlowManagerDelegate>)delegate flowManager:(struct flowmgr *)flowManager mediaManager:(id)mediaManager;
- (BOOL)isReady;

+ (instancetype)getInstance;

- (NSArray *)events;

- (void)appendLogForConversation:(NSString *)convid message:(NSString *)msg;

- (void)processResponseWithStatus:(int)status
                 reason:(NSString *)reason
              mediaType:(NSString *)mtype
                content:(NSData *)content
                context:(void const *)ctx;

- (BOOL)processEventWithMediaType:(NSString *)mtype content:(NSData *)content;

- (BOOL)acquireFlows:(NSString *)convId;

- (void)releaseFlows:(NSString *)convId;

- (void)setActive:(NSString *)convId active:(BOOL)active;
- (void)addUser:(NSString *)convId userId:(NSString *)userId
           name:(NSString *)name;
- (void)setSelfUser:(NSString *)userId;
- (void)refreshAccessToken:(NSString *)token type:(NSString *)type;


- (void)networkChanged;

- (void)callInterruptionStartInConversation:(NSString *)convId;
- (void)callInterruptionEndInConversation:(NSString *)convId;


- (int)ausrcChanged:(enum AVSFlowManagerAudioSource)ausrc;
- (int)auplayChanged:(enum AVSFlowManagerAudioPlay)aplay;

- (BOOL)isMuted;
- (int)setMute:(BOOL)muted;

- (NSArray *)sortConferenceParticipants:(NSArray *)participants;


- (void)mediaCategoryChanged:(NSString *)convId category:(AVSFlowManagerCategory)category;

- (void)playbackRouteDidChangeInMediaManager:(AVSPlaybackRoute)play_back_route;

- (BOOL)isMediaEstablishedInConversation:(NSString *)convId;

- (void)updateModeInConversation:(NSString *)convId withCategory:(AVSFlowManagerCategory)category;
- (void)updateVolumeForUser:(NSString *)userid inVol:(float)input outVol:(float)output inConversation:(NSString *)convId;

- (void)handleError:(int)error inConversation:(NSString *)convId;
- (void)mediaEstablishedInConversation:(NSString *)convId;
- (void)conferenceParticipants:(NSArray *)participants inConversation:(NSString *)convId;

- (void)setEnableLogging:(BOOL)enable;
- (void)setEnableMetrics:(BOOL)enable;

- (void)setSessionId:(NSString *)sessId forConversation:(NSString *)convId;

- (BOOL)canSendVideoForConversation:(NSString *)convId;
- (BOOL)isSendingVideoInConversation:(NSString *)convId
                      forParticipant:(NSString *)partId;
- (void)setVideoSendState:(AVSFlowManagerVideoSendState)state forConversation:(NSString *)convId;
- (void)setVideoPreview:(UIView *)view forConversation:(NSString *)convId;
- (void)setVideoView:(UIView *)view forConversation:(NSString *)convId forParticipant:(NSString *)partId;

- (NSArray*)getVideoCaptureDevices;
- (void)setVideoCaptureDevice:(NSString *)deviceId forConversation:(NSString *)convId;

- (void)vmStartRecord:(NSString *)fileName;
- (void)vmStopRecord;
- (int)vmGetLength:(NSString *)fileName;
- (void)vmStartPlay:(NSString *)fileName toStart:(int)startpos;
- (void)vmStopPlay;

@end

