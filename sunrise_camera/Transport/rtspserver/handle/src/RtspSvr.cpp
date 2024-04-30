#include "RtspSvr.hh"
#include "utils/utils_log.h"
#include "utils/common_utils.h"
#include "rtsp_server_default_param.h"

CRtspServer* CRtspServer::instance = NULL;

static int const samplingFrequencyTable[16] =
{
	96000, 88200, 64000, 48000,
	44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000,
	7350, 0, 0, 0
};

static int GetSamplingFrequencyIndex(int sampleate)
{
	int index = 0;
	unsigned int i = 0;
	for(i = 0; i < ARRAY_SIZE(samplingFrequencyTable); i++)
	{
		if(samplingFrequencyTable[i] == sampleate)
		{
			index = i;
			break;
		}
	}

	return index;
}

CRtspServer* CRtspServer::GetInstance()
{
	if(instance == NULL)
	{
		instance = new CRtspServer();
	}

	return instance;
}

CRtspServer::CRtspServer()
	:m_Stop(true),m_watchVariable(0),m_scheduler(NULL),m_env(NULL),
	m_rtspServer(NULL),m_pThread(0)
{

}

CRtspServer::~CRtspServer()
{
	if(m_env == NULL && m_scheduler == NULL)
	{
		Destory();
	}
}

void * CRtspServer::ThreadRtspServerProcImpl(void* arg)
{
	CRtspServer* cRtspServer = (CRtspServer*)arg;

	cRtspServer->ThreadRtspServer();

	return NULL;
}

void CRtspServer::ThreadRtspServer()
{
	/*Boolean reuseFirstSource = False;*/

	UserAuthenticationDatabase* authDB = NULL;
#ifdef ACCESS_CONTROL
	authDB = new UserAuthenticationDatabase;
	authDB->addUserRecord("admin", "admin123"); // replace these with real strings
#endif

	do{
		portNumBits rtspServerPortNum = m_port;
		m_rtspServer = RTSPServer::createNew(*m_env, rtspServerPortNum, authDB);
		if (m_rtspServer == NULL) {
			SC_LOGE("m_rtspServer create error, port:%d", rtspServerPortNum);
			break;
		}
		SC_LOGI("CRtspServer Start At Port: %d", m_port);
		m_Stop = false;
		m_env->taskScheduler().doEventLoop(&m_watchVariable); // does not return
	}while(0);

#ifdef ACCESS_CONTROL
	delete authDB;
#endif
}

bool CRtspServer::Create(portNumBits port)
{
	m_port = port;

	/*SC_LOGE("CRtspServer creat port:%d\n", port);*/

	if(m_env != NULL && m_scheduler != NULL)
	{

		SC_LOGE("CRtspServer is already Create yet");
		return false;
	}

	m_scheduler = BasicTaskScheduler::createNew();
	m_env = BasicUsageEnvironment::createNew(*m_scheduler);

	return true;
}

bool CRtspServer::Destory()
{
	if(!m_Stop)
	{
		Stop();
	}

	if(m_env == NULL && m_scheduler == NULL)
	{
		SC_LOGE("CRtspServer is already Destory yet, call Destory error!");
		return false;
	}

	m_env->reclaim(); m_env = NULL;
    delete m_scheduler; m_scheduler = NULL;

	return true;
}

bool CRtspServer::Start()
{

	if(m_env == NULL || m_scheduler == NULL)
	{
		SC_LOGE("CRtspServer is not Create yet, call Create first!");
		return false;
	}

	if(!m_Stop)
	{
		SC_LOGE("CRtspServer is already start, call Start error!");
		return false;
	}

	m_watchVariable = 0;
	if(pthread_create(&m_pThread,NULL, ThreadRtspServerProcImpl, this))
	{
		SC_LOGE("ThreadRtspServerProcImpl false!\n");
		return false;
	}

	// 等待线程执行完成，代表rtsp server已经启动完成
	while(m_Stop) usleep(5);

	return true;
}

bool CRtspServer::Stop()
{
	if(m_Stop)
	{
		SC_LOGE("CRtspServer is stop, call stop error!");
		return false;
	}

	m_watchVariable = 1;
	::pthread_join(m_pThread, 0);
	m_pThread = 0;
	Medium::close(m_rtspServer); m_rtspServer = NULL;
	m_Stop = true;

	return true;
}

bool CRtspServer::Restart()
{
	bool ret = false;
	ret = Stop();
	if(!ret)
	{
		return ret;
	}
	ret = Start();

	return ret;
}

bool CRtspServer::DynamicAddSms(const char* streamName,
	bool audioEnable, int audioType, int audioSampleRate, int audioBitPerSample,
	int audioChannels, bool videoEnable, int videoType, int videoFrameRate,
	char *shmId, char *shmName, int streamBufSize, int frameRate)
{
	Boolean reuseFirstSource = False;
	OutPacketBuffer::maxSize = 4*1024*1024; // 此处的配置客户根据码流的分辨率和bitrate调整，避免内存浪费
	// A H.264 video elementary stream:
	ServerMediaSession* sms = ServerMediaSession::createNew(*m_env, streamName, streamName, "H.264 video elementary stream", True);
	if(videoEnable && videoType == RTSPSRV_VIDEO_TYPE_H264)
	{
		sms->addSubsession(H264VideoLiveServerMediaSubsession::createNew(*m_env, reuseFirstSource, shmId, shmName, streamBufSize, frameRate));
	}
	if(audioEnable)
	{
		int index = GetSamplingFrequencyIndex(audioSampleRate);
		if(audioType == RTSPSRV_AUDIO_TYPE_LPCM)
		{
			sms->addSubsession(LPCMAudioLiveServerMediaSubsession::createNew(*m_env, reuseFirstSource, audioBitPerSample, index, audioChannels));
		}
		else if(audioType == RTSPSRV_AUDIO_TYPE_PCMA)
		{
			sms->addSubsession(PCMAAudioLiveServerMediaSubsession::createNew(*m_env, reuseFirstSource, audioBitPerSample, index, audioChannels));
		}
	}

	m_rtspServer->addServerMediaSession(sms);

	char* url = m_rtspServer->rtspURL(sms);

	SC_LOGI("Play <%s> stream using the URL %s", streamName, url);
	delete[] url;
	return true;
}

bool CRtspServer::DynamicDelSms(const char* streamName)
{
	ServerMediaSession* sms = m_rtspServer->lookupServerMediaSession(streamName);
	Boolean const smsExists = sms != NULL;

	if (smsExists) {
		// "sms" was created for a file that no longer exists. Remove it:
		m_rtspServer->removeServerMediaSession(sms);
		sms = NULL;
	}
	return true;
}

