/* Kernel Labs Inc - 2016/2017
 * Primitive support for capturing 1920x1080p60 from BlackMagic hardware.
 * Support is limit to 1080p60 as the integration between the
 * primary project framework and the decklink.cpp implementation is simplistic.
 * TODO: FLush out the command line interface to pass a wider set of
 * decklink options (such as being able to vary the video mode).
 *
 * The Decklink code was originally ripped from the Blackmagic SDK and added
 * here in as simple a way as possible. For sure, the glue between the core
 * "encoder" operations callbacks and the blackmagic controlling software
 * could do with some refinement.
 *
 * Why is the height 1088? Yeah, the encoder framework expects height to be a multiple
 * of 16. That's not a strict VAAPI requirement, that's probably a sign that our
 * YUV-to-VAAPI upload function is broken. However, for the time being, 1088
 * serves our purposes fine. TODO: I guess we could YUY2 blackout the last 8 lines.
 */

#define __STDC_CONSTANT_MACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include "capture.h"
#include "DeckLinkAPI.h"
#include "decklink.h"
#include "BMDConfig.h"
#include "DeckLinkAPIDispatch.cpp"

extern "C" {
#include <libswscale/swscale.h>
static struct SwsContext *encoderSwsContext = NULL;
};

//
static struct encoder_operations_s *encoder = 0;
static struct encoder_params_s *encoder_params = 0;
static int ipcFPS = 30;
static int ipcResubmitTimeoutMS = 0;
static int fixedWidth;
static int fixedHeight;

static int g_videoOutputFile = -1;
static int g_audioOutputFile = -1;

static BMDConfig g_config;

static IDeckLinkInput *g_deckLinkInput = NULL;

static unsigned long g_frameCount = 0;

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate():m_refCount(1)
{
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	return __sync_add_and_fetch(&m_refCount, 1);
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	int32_t newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
	if (newRefValue == 0) {
		delete this;
		return 0;
	}
	return newRefValue;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame, IDeckLinkAudioInputPacket *audioFrame)
{
	IDeckLinkVideoFrame *rightEyeFrame = NULL;
	IDeckLinkVideoFrame3DExtensions *threeDExtensions = NULL;
	void *frameBytes;
	void *audioFrameBytes;

	// Handle Video Frame
	if (videoFrame) {
		// If 3D mode is enabled we retreive the 3D extensions interface which gives.
		// us access to the right eye frame by calling GetFrameForRightEye() .
		if ((videoFrame->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions,
				    (void **)&threeDExtensions) != S_OK)
		    || (threeDExtensions->GetFrameForRightEye(&rightEyeFrame) !=
			S_OK)) {
			rightEyeFrame = NULL;
		}

		if (threeDExtensions)
			threeDExtensions->Release();

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
			printf("Frame received (#%lu) - No input signal detected\n", g_frameCount);
		} else {
			const char *timecodeString = NULL;
			if (g_config.m_timecodeFormat != 0) {
				IDeckLinkTimecode *timecode;
				if (videoFrame->GetTimecode(g_config.m_timecodeFormat, &timecode) == S_OK) {
					timecode->GetString(&timecodeString);
				}
			}

			printf("Frame received (#%lu) [%s] - %s - Size: %li bytes\n",
			     g_frameCount,
			     timecodeString != NULL ? timecodeString : "No timecode",
			     rightEyeFrame != NULL ? "Valid Frame (3D left/right)" :
			     "Valid Frame",
			     videoFrame->GetRowBytes() * videoFrame->GetHeight());

			if (timecodeString)
				free((void *)timecodeString);

			{	/* Colorspace convert then Pass it to the encoder as YUY2.
				 * TODO: We probably should be doing this in the encoder core.
				 * Simply Expose a UVYV colorspace, push the frame out to the core
				 * and let the core manage the complexity on behalf of a number
				 * of potential UVYV sources.
				 */
				void *p;
				videoFrame->GetBytes(&p);

				uint8_t *f = (uint8_t *)malloc(fixedWidth * 2 * fixedHeight);

				encoderSwsContext = sws_getCachedContext(encoderSwsContext,
					fixedWidth, fixedHeight, AV_PIX_FMT_UYVY422,
					fixedWidth, fixedHeight, AV_PIX_FMT_YUYV422, SWS_BICUBIC, NULL, NULL, NULL);

				uint8_t *src_slices[] = { (uint8_t *)p };
				uint8_t *dst_slices[] = { f };
				const int src_slices_stride[] = { fixedWidth * 2 };
				const int dst_slices_stride[] = { fixedWidth * 2 };
				sws_scale(encoderSwsContext,
					src_slices, src_slices_stride,
					0, fixedHeight,
					dst_slices, dst_slices_stride);

			        if (!encoder_encode_frame(encoder, encoder_params, (unsigned char *)f))
					time_to_quit = 1;
				free(f);
			}

			if (g_videoOutputFile != -1) {
				videoFrame->GetBytes(&frameBytes);
				write(g_videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());

				if (rightEyeFrame) {
					rightEyeFrame->GetBytes(&frameBytes);
					write(g_videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
				}
			}
		}

		if (rightEyeFrame)
			rightEyeFrame->Release();

		g_frameCount++;
	}
	// Handle Audio Frame
	if (audioFrame) {
		if (g_audioOutputFile != -1) {
			audioFrame->GetBytes(&audioFrameBytes);
			write(g_audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() *
			      g_config.m_audioChannels *
			      (g_config.m_audioSampleDepth / 8));
		}
	}

	if (g_config.m_maxFrames > 0 && videoFrame && g_frameCount >= g_config.m_maxFrames) {
		time_to_quit = true;
	}

	return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode * mode,
			BMDDetectedVideoInputFormatFlags formatFlags)
{
	// This only gets called if bmdVideoInputEnableFormatDetection was set
	// when enabling video input
	HRESULT result;
	char *displayModeName = NULL;
	BMDPixelFormat pixelFormat = bmdFormat8BitYUV;

	if (formatFlags & bmdDetectedVideoInputRGB444)
		pixelFormat = bmdFormat10BitRGB;

	mode->GetName((const char **)&displayModeName);
	printf("Video format changed to %s %s\n", displayModeName,
	       formatFlags & bmdDetectedVideoInputRGB444 ? "RGB" : "YUV");

	if (displayModeName)
		free(displayModeName);

	if (g_deckLinkInput) {
		g_deckLinkInput->StopStreams();

		result = g_deckLinkInput->EnableVideoInput(mode->GetDisplayMode(),
						      pixelFormat,
						      g_config.m_inputFlags);
		if (result != S_OK) {
			fprintf(stderr, "Failed to switch video mode\n");
			goto bail;
		}

		g_deckLinkInput->StartStreams();
	}

bail:
	return S_OK;
}

int decklink_main(int argc, const char *arv[])
{
	HRESULT result;
	int exitStatus = 1;
	int idx;

	IDeckLinkIterator *deckLinkIterator = NULL;
	IDeckLink *deckLink = NULL;

	IDeckLinkAttributes *deckLinkAttributes = NULL;
	bool formatDetectionSupported;

	IDeckLinkDisplayModeIterator *displayModeIterator = NULL;
	IDeckLinkDisplayMode *displayMode = NULL;
	char *displayModeName = NULL;
	BMDDisplayModeSupport displayModeSupported;

	DeckLinkCaptureDelegate *delegate = NULL;

	// Process the command line arguments
	if (!g_config.ParseArguments(argc, (char **)arv)) {
		g_config.DisplayUsage(exitStatus);
		goto bail;
	}
	// Get the DeckLink device
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (!deckLinkIterator) {
		fprintf(stderr,
			"This application requires the DeckLink drivers installed.\n");
		goto bail;
	}

	idx = g_config.m_deckLinkIndex;

	while ((result = deckLinkIterator->Next(&deckLink)) == S_OK) {
		if (idx == 0)
			break;
		--idx;

		deckLink->Release();
	}

	if (result != S_OK || deckLink == NULL) {
		fprintf(stderr, "Unable to get DeckLink device %u\n",
			g_config.m_deckLinkIndex);
		goto bail;
	}
	// Get the input (capture) interface of the DeckLink device
	result = deckLink->QueryInterface(IID_IDeckLinkInput,
				     (void **)&g_deckLinkInput);
	if (result != S_OK)
		goto bail;

	// Get the display mode
	if (g_config.m_displayModeIndex == -1) {
		// Check the card supports format detection
		result = deckLink->QueryInterface(IID_IDeckLinkAttributes,
					     (void **)&deckLinkAttributes);
		if (result == S_OK) {
			result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection,
				    &formatDetectionSupported);
			if (result != S_OK || !formatDetectionSupported) {
				fprintf(stderr,
					"Format detection is not supported on this device\n");
				goto bail;
			}
		}

		g_config.m_inputFlags |= bmdVideoInputEnableFormatDetection;

		// Format detection still needs a valid mode to start with
		idx = 0;
	} else {
		idx = g_config.m_displayModeIndex;
	}

	result = g_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
		goto bail;

	while ((result = displayModeIterator->Next(&displayMode)) == S_OK) {
		if (idx == 0)
			break;
		--idx;

		displayMode->Release();
	}

	if (result != S_OK || displayMode == NULL) {
		fprintf(stderr, "Unable to get display mode %d\n",
			g_config.m_displayModeIndex);
		goto bail;
	}
	// Get display mode name
	result = displayMode->GetName((const char **)&displayModeName);
	if (result != S_OK) {
		displayModeName = (char *)malloc(32);
		snprintf(displayModeName, 32, "[index %d]",
			 g_config.m_displayModeIndex);
	}
	// Check display mode is supported with given options
	result = g_deckLinkInput->DoesSupportVideoMode(displayMode->GetDisplayMode(),
						  g_config.m_pixelFormat,
						  bmdVideoInputFlagDefault,
						  &displayModeSupported, NULL);
	if (result != S_OK)
		goto bail;

	if (displayModeSupported == bmdDisplayModeNotSupported) {
		fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n",
			displayModeName);
		goto bail;
	}

	if (g_config.m_inputFlags & bmdVideoInputDualStream3D) {
		if (!(displayMode->GetFlags() & bmdDisplayModeSupports3D)) {
			fprintf(stderr, "The display mode %s is not supported with 3D\n",
				displayModeName);
			goto bail;
		}
	}
	// Print the selected configuration
	g_config.DisplayConfiguration();

	// Configure the capture callback
	delegate = new DeckLinkCaptureDelegate();
	g_deckLinkInput->SetCallback(delegate);

	// Open output files
	if (g_config.m_videoOutputFile != NULL) {
		g_videoOutputFile = open(g_config.m_videoOutputFile,
			 O_WRONLY | O_CREAT | O_TRUNC, 0664);
		if (g_videoOutputFile < 0) {
			fprintf(stderr,
				"Could not open video output file \"%s\"\n",
				g_config.m_videoOutputFile);
			goto bail;
		}
	}

	if (g_config.m_audioOutputFile != NULL) {
		g_audioOutputFile = open(g_config.m_audioOutputFile,
			 O_WRONLY | O_CREAT | O_TRUNC, 0664);
		if (g_audioOutputFile < 0) {
			fprintf(stderr,
				"Could not open audio output file \"%s\"\n",
				g_config.m_audioOutputFile);
			goto bail;
		}
	}
	// Block main thread until signal occurs
	while (!time_to_quit) {
		// Start capturing
		result = g_deckLinkInput->EnableVideoInput(displayMode->
						      GetDisplayMode(),
						      g_config.m_pixelFormat,
						      g_config.m_inputFlags);
		if (result != S_OK) {
			fprintf(stderr,
				"Failed to enable video input. Is another application using the card?\n");
			goto bail;
		}

		result = g_deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz,
						      g_config.
						      m_audioSampleDepth,
						      g_config.m_audioChannels);
		if (result != S_OK)
			goto bail;

		result = g_deckLinkInput->StartStreams();
		if (result != S_OK)
			goto bail;

		// All Okay.
		exitStatus = 0;
	
		while (!time_to_quit) {	
			usleep(250 * 1000);
		}

		fprintf(stderr, "Decklink stopping hardware\n");
		g_deckLinkInput->StopStreams();
		g_deckLinkInput->DisableAudioInput();
		g_deckLinkInput->DisableVideoInput();
		fprintf(stderr, "Decklink stopped hardware\n");
	}
	fprintf(stderr, "Decklink main teardown\n");

bail:
	if (g_videoOutputFile != 0)
		close(g_videoOutputFile);

	if (g_audioOutputFile != 0)
		close(g_audioOutputFile);

	if (displayModeName != NULL)
		free(displayModeName);

	if (displayMode != NULL)
		displayMode->Release();

	if (displayModeIterator != NULL)
		displayModeIterator->Release();

	if (delegate != NULL)
		delegate->Release();

	if (g_deckLinkInput != NULL) {
		g_deckLinkInput->Release();
		g_deckLinkInput = NULL;
	}

	if (deckLinkAttributes != NULL)
		deckLinkAttributes->Release();

	if (deckLink != NULL)
		deckLink->Release();

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();

	return exitStatus;
}

static void decklink_stop_capturing(void)
{
}

static int decklink_start_capturing(struct encoder_operations_s *e)
{
        if (!e)
                return -1;

        encoder = e;
        return 0;
}

static void decklink_uninit_device(void)
{
}

static int decklink_init_device(struct encoder_params_s *p, struct capture_parameters_s *c)
{
        c->width = fixedWidth;
        c->height = fixedHeight;
        ipcFPS = c->fps;
        encoder_params = p;

        /* Lets give the timeout a small amount of headroom for a frame to arrive (3ms) */
        /* 30fps input creates a timeout of 36ms or as low as 27.7 fps, before we resumbit a prior frame. */
        printf("%s(%d, %d, %d timeout=%d)\n", __func__,
                c->width, c->height,
                ipcFPS, ipcResubmitTimeoutMS);
        ipcResubmitTimeoutMS = (1000 / ipcFPS) + 3;

        encoder_params->input_fourcc = E_FOURCC_YUY2;

        return 0;
}

static void decklink_close_device(void)
{
}

static int decklink_open_device()
{
        if ((fixedWidth != 1920) || (fixedHeight != 1088)) {
                fprintf(stderr, "fixed frame size miss-match\n");
                exit(1);
        }

        return 0;
}

static void decklink_set_defaults(struct capture_parameters_s *c)
{
	c->type = CM_DECKLINK;
	fixedWidth = 1920;
	fixedHeight = 1088;
}

static void decklink_mainloop(void)
{
	/* This array construction is hokey.
	 * I think decklink_main ends up calling getopt
	 * and has a references to the original argv/argc.
	 * Simply passing a fully formed argv[] with proper
	 * arg alignment creates parsing issues in BMDConfig.
	 * So, for the time being, because time is limited,
	 * I'm padding the array below to work around the issue.
	 *
	 * I test with:
	 * ./h264encoder -M4 -o raw.nals --intra_period 60 --bitrate 20000000
	 */
	char source_nr[26];
	sprintf(source_nr, "-d %d", encoder_params->source_nr);
	const char *argsX[] = {
		"h264encoder",
		"h264encoder",
		"h264encoder",
		"h264encoder",
		"h264encoder",
		source_nr,  /* input #0 */
		"-p 0",     /* 8 bit */
		"-p 0",     /* 8 bit */
		"-m 12",    /* 1080p60 */
		source_nr,  /* input #0 */
		"-p 0",     /* 8 bit */
		NULL,
	};

	decklink_main(11, &argsX[0]);

	fprintf(stderr, "Decklink stopped main\n");
}

struct capture_operations_s decklink_ops =
{
	.type		= CM_DECKLINK,
	.name		= (char *)"Decklink SDI (HD)",
	.set_defaults	= decklink_set_defaults,
	.mainloop	= decklink_mainloop,
	.stop		= decklink_stop_capturing,
	.start		= decklink_start_capturing,
	.uninit		= decklink_uninit_device,
	.init		= decklink_init_device,
	.close		= decklink_close_device,
	.open		= decklink_open_device,
	.default_fps	= 60,
};

