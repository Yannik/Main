/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** main.cpp
** 
** V4L2 RTSP streamer                                                                 
**                                                                                    
** H264 capture using V4L2                                                            
** RTSP using live555                                                                 
**                                                                                    
** -------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <dirent.h>

#include <sstream>

// libv4l2
#include <linux/videodev2.h>

// live555
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>

// project
#include "logger.h"

#include "V4l2Device.h"
#include "V4l2Capture.h"
#include "V4l2Output.h"

#include "ImpCapture.h"
#include "ImpEncoder.h"

#include "H264_V4l2DeviceSource.h"
#include "ServerMediaSubsession.h"
#include "UnicastServerMediaSubsession.h"
#include "MulticastServerMediaSubsession.h"
#include "SegmentServerMediaSubsession.h"
#include "HTTPServer.h"
#include "ConfigReader.h"

#include "ALSACapture.h"

#define LOGURU_IMPLEMENTATION 1
#include "loguru.hpp"

// -----------------------------------------
//    signal handler
// -----------------------------------------
char quit = 0;

void sighandler(int n) {
    LOG_S(ERROR) << "SIGINT";
    quit = 1;
}

DetectionSaveToDiskState flushBufferToFile = BUFFERIZE;
void sighandlerUsr1(int n) {
   LOG_S(INFO) << "SIGUSR1";
   if (flushBufferToFile == BUFFERIZE)
   {
        LOG_S(INFO) << "SIGEVENT: buffer to disk";
        flushBufferToFile = LIVETODISK;
   } else  {
        LOG_S(INFO) << "SIGEVENT: End write to disk";
        flushBufferToFile = BUFFERIZE;
   }


    signal(SIGUSR1, sighandlerUsr1);
}
// -----------------------------------------
//    create UserAuthenticationDatabase for RTSP server
// -----------------------------------------
UserAuthenticationDatabase *
createUserAuthenticationDatabase(const std::list <std::string> &userPasswordList, const char *realm) {
    UserAuthenticationDatabase *auth = NULL;
    if (userPasswordList.size() > 0) {
        auth = new UserAuthenticationDatabase(realm, (realm != NULL));

        std::list<std::string>::const_iterator it;
        for (it = userPasswordList.begin(); it != userPasswordList.end(); ++it) {
            std::istringstream is(*it);
            std::string user;
            getline(is, user, ':');
            std::string password;
            getline(is, password);
            auth->addUserRecord(user.c_str(), password.c_str());
        }
    }

    return auth;
}

// -----------------------------------------
//    create RTSP server
// -----------------------------------------
RTSPServer *
createRTSPServer(UsageEnvironment &env, unsigned short rtspPort, unsigned short rtspOverHTTPPort, int timeout,
                 unsigned int hlsSegment, const std::list <std::string> &userPasswordList, const char *realm) {
    UserAuthenticationDatabase *auth = createUserAuthenticationDatabase(userPasswordList, realm);
    RTSPServer *rtspServer = HTTPServer::createNew(env, rtspPort, auth, timeout, hlsSegment);
    if (rtspServer != NULL) {
        // set http tunneling
        if (rtspOverHTTPPort) {
            rtspServer->setUpTunnelingOverHTTP(rtspOverHTTPPort);
        }
    }
    return rtspServer;
}


// -----------------------------------------
//    create FramedSource server
// -----------------------------------------
FramedSource *
createFramedSource(UsageEnvironment *env, int format, DeviceInterface *videoCapture, int outfd, int queueSize,
                   bool useThread, bool repeatConfig, MPEG2TransportStreamFromESSource *muxer) {
    bool muxTS = (muxer != NULL);
    FramedSource *source = NULL;
    if (format == V4L2_PIX_FMT_H264) {
        source = H264_V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread, repeatConfig, muxTS);
        if (muxTS) {
            muxer->addNewVideoSource(source, 5);
            source = muxer;
        }
    } else if (format == V4L2_PIX_FMT_HEVC) {
        source = H265_V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread, repeatConfig, muxTS);
        if (muxTS) {
            muxer->addNewVideoSource(source, 6);
            source = muxer;
        }
    } else {
        source = V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread);
        //source = ImpJpegDeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread);
    }
    return source;
}

// -----------------------------------------
//    add an RTSP session
// -----------------------------------------
int addSession(RTSPServer *rtspServer, const std::string &sessionName,
               const std::list<ServerMediaSubsession *> &subSession) {
    int nbSubsession = 0;
    if (subSession.empty() == false) {
        UsageEnvironment &env(rtspServer->envir());
        ServerMediaSession *sms = ServerMediaSession::createNew(env, sessionName.c_str());
        if (sms != NULL) {
            std::list<ServerMediaSubsession *>::const_iterator subIt;
            for (subIt = subSession.begin(); subIt != subSession.end(); ++subIt) {
                sms->addSubsession(*subIt);
                nbSubsession++;
            }

            rtspServer->addServerMediaSession(sms);

            char *url = rtspServer->rtspURL(sms);
            if (url != NULL) {
                LOG_S(INFO) << "Play this stream using the URL \"" << url << "\"" ;
                delete[] url;
            }
        }
    }
    return nbSubsession;
}

// -----------------------------------------
//    convert V4L2 pix format to RTP mime
// -----------------------------------------
std::string getRtpFormat(int format, bool muxTS) {
    std::string rtpFormat;
    if (muxTS) {
        rtpFormat = "video/MP2T";
    } else {
        switch (format) {
            case V4L2_PIX_FMT_HEVC :
                rtpFormat = "video/H265";
                break;
            case V4L2_PIX_FMT_H264 :
                rtpFormat = "video/H264";
                break;
            case V4L2_PIX_FMT_MJPEG:
                rtpFormat = "video/JPEG";
                break;
            case V4L2_PIX_FMT_JPEG :
                rtpFormat = "video/JPEG";
                break;
            case V4L2_PIX_FMT_VP8  :
                rtpFormat = "video/VP8";
                break;
            case V4L2_PIX_FMT_VP9  :
                rtpFormat = "video/VP9";
                break;
        }
    }

    return rtpFormat;
}

// -----------------------------------------
//    convert string video format to fourcc 
// -----------------------------------------
int decodeVideoFormat(const char *fmt) {
    char fourcc[4];
    memset(&fourcc, 0, sizeof(fourcc));
    if (fmt != NULL) {
        strncpy(fourcc, fmt, 4);
    }
    return v4l2_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}


//"EncodeFormat:InSampleRate:OutSampleRate"
void decodeEncodeFormat(const std::string &in, audioencoding &format, int &inAudioFreq, int &outAudioFreq )
{
  std::istringstream is(in);
  std::string form("MP3");
  getline(is, form, ':');
  std::string inSampleRate("44100");
  getline(is, inSampleRate, ':');
  std::string outSampleRate("44100");
  getline(is, outSampleRate, ':');

  if (!form.empty()) {
        if (form.find("OPUS") ==0)
        {
            format = ENCODE_OPUS;
        }
        else if (form.find("PCMU") ==0)
        {
            format = ENCODE_ULAW;
        }
        else if (form.find("PCM") ==0)
        {
            format = ENCODE_PCM;
        }
        else {
            format = ENCODE_MP3;
        }
    }

    if (inSampleRate.length() > 0)
        inAudioFreq =  std::stoi(inSampleRate);

    if (outSampleRate.length() > 0)
        outAudioFreq =  std::stoi(outSampleRate);
}



// -------------------------------------------------------
//    decode multicast url <group>:<rtp_port>:<rtcp_port>
// -------------------------------------------------------
void decodeMulticastUrl(const std::string &maddr, in_addr &destinationAddress, unsigned short &rtpPortNum,
                        unsigned short &rtcpPortNum) {
    std::istringstream is(maddr);
    std::string ip;
    getline(is, ip, ':');
    if (!ip.empty()) {
        destinationAddress.s_addr = inet_addr(ip.c_str());
    }

    std::string port;
    getline(is, port, ':');
    rtpPortNum = 20000;
    if (!port.empty()) {
        rtpPortNum = atoi(port.c_str());
    }
    rtcpPortNum = rtpPortNum + 1;
}

// -------------------------------------------------------
//    split video,audio device
// -------------------------------------------------------
void decodeDevice(const std::string &device, std::string &videoDev, std::string &audioDev) {
    std::istringstream is(device);
    getline(is, videoDev, ',');
    getline(is, audioDev);
}

std::string getDeviceName(const std::string &devicePath) {
    std::string deviceName(devicePath);
    size_t pos = deviceName.find_last_of('/');
    if (pos != std::string::npos) {
        deviceName.erase(0, pos + 1);
    }
    return deviceName;
}



// -----------------------------------------
//    entry point
// -----------------------------------------
int main(int argc, char **argv, char**environ) {
    // default parameters

    ConfigReader::instance().readConfig();


    bool disableAudio = ! ConfigReader::instance().getAudioEnabled();
    bool alsaAudio = false;
    int format = V4L2_PIX_FMT_H264;
    int width = ConfigReader::instance().getWidth();
    int height = ConfigReader::instance().getHeight();
    int queueSize = 10;
    int fps = ConfigReader::instance().getFps();
    int rcmode = ENC_RC_MODE_VBR;
    unsigned short rtspPort = 8554;
    unsigned short rtspOverHTTPPort = 0;
    bool multicast = false;
    int verbose = 0;
    std::string outputFile;
    std::string url = "unicast";
    std::string murl = "multicast";
    bool useThread = true;
    std::string maddr;
    bool repeatConfig = true;
    int timeout = 65;
    bool muxTS = false;
    unsigned int hlsSegment = 0;
    const char *realm = NULL;
    std::list <std::string> userPasswordList;
    int inAudioFreq = 44100;
    int outAudioFreq = 44100;
    audioencoding encode = ENCODE_MP3;


    const char *defaultPort = getenv("PORT");
    if (defaultPort != NULL) {
        rtspPort = atoi(defaultPort);
    }
    loguru::set_thread_name("main thread");
    // decode parameters






    // create live555 environment
    TaskScheduler *scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);

    // split multicast info
    struct in_addr destinationAddress;
    destinationAddress.s_addr = chooseRandomIPv4SSMAddress(*env);
    unsigned short rtpPortNum = 20000;
    unsigned short rtcpPortNum = rtpPortNum + 1;
    unsigned char ttl = 5;
    decodeMulticastUrl(maddr, destinationAddress, rtpPortNum, rtcpPortNum);

    // create RTSP server
    RTSPServer *rtspServer = createRTSPServer(*env, rtspPort, rtspOverHTTPPort, timeout, hlsSegment, userPasswordList,
                                              realm);
    if (rtspServer == NULL) {
        LOG_S(ERROR) << "Failed to create RTSP server: " << env->getResultMsg();
    } else {
        V4l2Output *out = NULL;
        int nbSource = 0;
        std::string baseUrl;

        MPEG2TransportStreamFromESSource *muxer = NULL;
        if (muxTS) {
            muxer = MPEG2TransportStreamFromESSource::createNew(*env);
        }
        StreamReplicator *videoReplicator = NULL;
        std::string rtpFormat;


        int outfd = -1;
        //int videoFormat = V4L2_PIX_FMT_MJPEG;
        int videoFormat = format;


        impParams params;
        params.width = width;
        params.height = height;
        params.rcmode = rcmode;
        
        if (videoFormat == V4L2_PIX_FMT_MJPEG) {
            params.mode = IMP_MODE_JPEG;
            OutPacketBuffer::maxSize = 250000;


        } else if (videoFormat == V4L2_PIX_FMT_H264) {
            params.mode = IMP_MODE_H264_SNAP;
            MPEG2TransportStreamFromESSource::maxInputESFrameSize += 4820;
            OutPacketBuffer::maxSize = 300000;

        } else {
            LOG_S(FATAL) << "Unrecognized Format ";
            exit(0);
        }
        if(width == 1920 && height == 1080){
            OutPacketBuffer::maxSize = 600000;
        } else  if(width == 1600 && height == 900){
            OutPacketBuffer::maxSize = 450000;
        }

        params.framerate = fps;

        // this is the default values, the real values are read from sharedmemory when
        // initializing the video ...
        params.bitrate = (double)2000.0 * (width * height) / (1280 * 720);;


        ImpCapture *impCapture = new ImpCapture(params);

        if (!outputFile.empty()) {
            if(strcmp(outputFile.c_str(),"memory")==0){
                outfd = -2;
            }else{
                outfd = (int)fopen(outputFile.c_str(),"w");
            }
        }



        rtpFormat.assign(getRtpFormat(videoFormat, muxTS));
        FramedSource *videoSource = createFramedSource(env, videoFormat,
                                                       new DeviceCaptureAccess<ImpCapture>(impCapture),
                                                       outfd, queueSize, useThread, repeatConfig, muxer);
        if (videoSource == NULL) {
            LOG_S(FATAL) << "Unable to create source for device ";
        } else {
            // extend buffer size if needed
            /*
            if (videoCapture->getBufferSize() > OutPacketBuffer::maxSize) {
                OutPacketBuffer::maxSize = videoCapture->getBufferSize();
            }
             */
            videoReplicator = StreamReplicator::createNew(*env, videoSource, false);
        }



        // Init Audio Capture
        StreamReplicator *audioReplicator = NULL;
        std::string rtpAudioFormat;
        std::string audioDev="/dev/dsp";
        //audioDev = "";
        if (disableAudio == false)
        {
            // find the ALSA device associated with the V4L2 device
            //audioDev = "";
            ALSACapture* audioCapture = NULL;
            // Init audio capture
            if (alsaAudio == true) {
                LOG_S(INFO)<< "Create ALSA audio Source..." << audioDev;
                ALSACaptureParameters param(audioDev.c_str(), inAudioFreq, outAudioFreq, verbose, encode, SOURCE_DSP);
                audioCapture = ALSACapture::createNew(param);

            } else {
                LOG_S(INFO)<< "Create SDK audio Source...";
                ALSACaptureParameters param(audioDev.c_str(), inAudioFreq, outAudioFreq, verbose, encode, SOURCE_IMP);
                audioCapture = ALSACapture::createNew(param);
            }
            if (audioCapture)
            {
                FramedSource* audioSource = V4L2DeviceSource::createNew(*env, new DeviceCaptureAccess<ALSACapture>(audioCapture), -1, queueSize, useThread);
                if (audioSource == NULL)
                {
                    LOG_S(FATAL) << "Unable to create source for device " << audioDev;
                    delete audioCapture;
                }
                else
                {
                    std::ostringstream os;
                    switch (encode)
                    {
                        case ENCODE_MP3:
                            os << "audio/MPEG";
                            break;
                        case ENCODE_OPUS:
                            outAudioFreq = 48000;
                            os << "audio/OPUS/" << outAudioFreq << "/1";
                            break;
                        case ENCODE_PCM:
                            outAudioFreq = inAudioFreq;
                            os << "audio/L16/" << outAudioFreq << "/1";
                            break;
                        case ENCODE_ULAW:
                            outAudioFreq = inAudioFreq;
                            os << "audio/PCMU/"  << outAudioFreq << "/1";;
                            break;
                    }
                    //os << "audio/L16/" << audioCapture->getSampleRate() << "/" << audioCapture->getChannels();
                    //os << "audio/L16/8000/1";
                    //os << "audio/MPEG";
                    rtpAudioFormat.assign(os.str());

                    // extend buffer size if needed
                    if (audioCapture->getBufferSize() > OutPacketBuffer::maxSize)
                    {
                        OutPacketBuffer::maxSize = audioCapture->getBufferSize();
                    }
                    audioReplicator = StreamReplicator::createNew(*env, audioSource, false);
                }
            }
        }


        // Create Multicast Session
        if (multicast) {
            LOG_S(INFO) << "RTP  address " << inet_ntoa(destinationAddress) << ":" << rtpPortNum;
            LOG_S(INFO) << "RTCP address " << inet_ntoa(destinationAddress) << ":" << rtcpPortNum;

            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(
                        MulticastServerMediaSubsession::createNew(*env, destinationAddress, Port(rtpPortNum),
                                                                  Port(rtcpPortNum), ttl, videoReplicator, rtpFormat));
                // increment ports for next sessions
                rtpPortNum += 2;
                rtcpPortNum += 2;
            }

            if (audioReplicator) {
                subSession.push_back(
                        MulticastServerMediaSubsession::createNew(*env, destinationAddress, Port(rtpPortNum),
                                                                  Port(rtcpPortNum), ttl, audioReplicator,
                                                                  rtpAudioFormat));

                // increment ports for next sessions
                rtpPortNum += 2;
                rtcpPortNum += 2;
            }
            nbSource += addSession(rtspServer, baseUrl + murl, subSession);
        }
        // Create Unicast Session
        if (hlsSegment > 0) {
            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(HLSServerMediaSubsession::createNew(*env, videoReplicator, rtpFormat, hlsSegment));
            }
            nbSource += addSession(rtspServer, baseUrl + url, subSession);

            struct in_addr ip;
            ip.s_addr = ourIPAddress(*env);
            LOG_S(INFO) << "HLS       http://" << inet_ntoa(ip) << ":" << rtspPort << "/" << baseUrl + url << ".m3u8";
            LOG_S(INFO) << "MPEG-DASH http://" << inet_ntoa(ip) << ":" << rtspPort << "/" << baseUrl + url << ".mpd";
        } else {
            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(UnicastServerMediaSubsession::createNew(*env, videoReplicator, rtpFormat));
            }
            if (audioReplicator) {
                subSession.push_back(UnicastServerMediaSubsession::createNew(*env, audioReplicator, rtpAudioFormat));
            }
            nbSource += addSession(rtspServer, baseUrl + url, subSession);
        }


        if (nbSource > 0) {
            // main loop
            signal(SIGINT, sighandler);
            signal(SIGUSR1, sighandlerUsr1);

            env->taskScheduler().doEventLoop(&quit);
            LOG_S(INFO) << "Exiting....";
        }

        Medium::close(rtspServer);

        if (out) {
            delete out;
        }
    }

    env->reclaim();
    delete scheduler;

    return 0;
}



