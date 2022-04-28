
/*
* UAE
*
* Win32 audio player for CDA emulation
*
* Copyright 2010 Toni Wilen
*
*/

#define CDADS 0

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "dxwrap.h"
#include "audio.h"
#include "blkdev.h"
#include "threaddep/thread.h"

#include <dsound.h>
#include <mmreg.h>
#include <sys/timeb.h>

#include "win32.h"
#include "gui.h"

#include "cda_play.h"

cda_audio::~cda_audio()
{
	if (active) {
		wait(0);
		wait(1);
#if CDADS
		if (dsnotify)
			dsnotify->Release();
		if (dsbuf)
			dsbuf->Release();
		if (ds)
			ds->Release();
		if (notifyevent[0])
			CloseHandle(notifyevent[0]);
		if (notifyevent[1])
			CloseHandle(notifyevent[1]);
#else
		for (int i = 0; i < 2; i++)
			waveOutUnprepareHeader(wavehandle, &whdr[i], sizeof(WAVEHDR));
		if (wavehandle != NULL)
			waveOutClose(wavehandle);
#endif
	}
	for (int i = 0; i < 2; i++) {
		xfree (buffers[i]);
		buffers[i] = NULL;
	}
}

cda_audio::cda_audio(int num_sectors, int sectorsize, int samplerate, bool internalmode)
{
	active = false;
	playing = false;
	volume[0] = volume[1] = 0;

	bufsize = num_sectors * sectorsize;
	this->sectorsize = sectorsize;
	for (int i = 0; i < 2; i++) {
		buffers[i] = xcalloc (uae_u8, num_sectors * ((bufsize + 4095) & ~4095));
	}
	this->num_sectors = num_sectors;

	if (internalmode)
		return;

	WAVEFORMATEX wav;
	memset (&wav, 0, sizeof (WAVEFORMATEX));

	wav.cbSize = 0;
	wav.nChannels = 2;
	wav.nSamplesPerSec = samplerate;
	wav.wBitsPerSample = 16;
	wav.nBlockAlign = wav.wBitsPerSample / 8 * wav.nChannels;
	wav.nAvgBytesPerSec = wav.nBlockAlign * wav.nSamplesPerSec;
	wav.wFormatTag = WAVE_FORMAT_PCM;

#if CDADS
	LPDIRECTSOUNDBUFFER pdsb;
	WAVEFORMATEXTENSIBLE wavfmt;
	DSBUFFERDESC desc;
	HRESULT hr;

	dsnotify = NULL;
	dsbuf = NULL;
	ds = NULL;
	notifyevent[0] = notifyevent[1] = NULL;

	hr = DirectSoundCreate8 (&sound_devices[currprefs.win32_soundcard].guid, &ds, NULL);
	if (FAILED (hr))  {
		write_log (_T("CDA: DirectSoundCreate8() failure: %s\n"), DXError (hr));
		return;
	}

	hr = ds->SetCooperativeLevel (hMainWnd, DSSCL_PRIORITY);
	if (FAILED (hr)) {
		write_log (_T("CDA: Can't set cooperativelevel: %s\n"), DXError (hr));
		return;
	}

	wavfmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wavfmt.Format.nChannels = 2;
	wavfmt.Format.nSamplesPerSec = 44100;
	wavfmt.Format.wBitsPerSample = 16;
	wavfmt.Format.nBlockAlign = wavfmt.Format.wBitsPerSample / 8 * wavfmt.Format.nChannels;
	wavfmt.Format.nAvgBytesPerSec = wavfmt.Format.nBlockAlign * wavfmt.Format.nSamplesPerSec;
	wavfmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	wavfmt.Samples.wValidBitsPerSample = 16;
	wavfmt.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);

	memset (&desc, 0, sizeof desc);
	desc.dwSize = sizeof desc;
	desc.dwBufferBytes = 2 * bufsize;
	desc.lpwfxFormat = &wavfmt.Format;
	desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
	desc.dwFlags |= DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME;
	desc.guid3DAlgorithm = GUID_NULL;

	hr = ds->CreateSoundBuffer (&desc, &pdsb, NULL);
	if (FAILED (hr)) {
		write_log (_T("CDA: IDirectSound_CreateSoundBuffer %s\n"), DXError (hr));
		return;
	}
	hr = pdsb->QueryInterface (IID_IDirectSoundBuffer8, (LPVOID*)&dsbuf);
	IDirectSound_Release (pdsb);
	if (FAILED (hr))  {
		write_log (_T("CDA: Secondary QueryInterface() failure: %s\n"), DXError (hr));
		return;
	}
	hr = dsbuf->QueryInterface (IID_IDirectSoundNotify, (LPVOID*)&dsnotify);
	if (FAILED (hr))  {
		write_log (_T("CDA: IID_IDirectSoundNotify QueryInterface() failure: %s\n"), DXError (hr));
		return;
	}

	notifyevent[0] = CreateEvent (NULL, TRUE, FALSE, NULL);
	notifyevent[1] = CreateEvent (NULL, TRUE, FALSE, NULL);
	DSBPOSITIONNOTIFY nf[2];
	nf[0].dwOffset = bufsize / num_sectors;
	nf[1].dwOffset = bufsize + bufsize / num_sectors;
	nf[0].hEventNotify = notifyevent[0];
	nf[1].hEventNotify = notifyevent[1];
	hr = dsnotify->SetNotificationPositions(2, nf);

	active = true;
#else
	MMRESULT mmr;
	mmr = waveOutOpen (&wavehandle, WAVE_MAPPER, &wav, 0, 0, WAVE_ALLOWSYNC | WAVE_FORMAT_DIRECT);
	if (mmr != MMSYSERR_NOERROR) {
		write_log (_T("IMAGE CDDA: wave open %d\n"), mmr);
		return;
	}
	for (int i = 0; i < 2; i++) {
		memset (&whdr[i], 0, sizeof(WAVEHDR));
		whdr[i].dwBufferLength = sectorsize * num_sectors;
		whdr[i].lpData = (LPSTR)buffers[i];
		mmr = waveOutPrepareHeader (wavehandle, &whdr[i], sizeof (WAVEHDR));
		if (mmr != MMSYSERR_NOERROR) {
			write_log (_T("IMAGE CDDA: waveOutPrepareHeader %d:%d\n"), i, mmr);
			return;
		}
		whdr[i].dwFlags |= WHDR_DONE;
	}
	active = true;
	playing = true;
#endif
}

void cda_audio::setvolume(int left, int right)
{
	for (int j = 0; j < 2; j++) {
		volume[j] = j == 0 ? left : right;
		volume[j] = sound_cd_volume[j] * volume[j] / 32768;
		if (volume[j])
			volume[j]++;
		volume[j] = volume[j] * (100 - currprefs.sound_volume_master) / 100;
		if (volume[j] >= 32768)
			volume[j] = 32768;
	}
#if CDADS
	LONG vol = DSBVOLUME_MIN;
	int volume = master * left / 32768;
	if (volume < 100)
		vol = (LONG)((DSBVOLUME_MIN / 2) + (-DSBVOLUME_MIN / 2) * log (1 + (2.718281828 - 1) * (1 - volume / 100.0)));
	HRESULT hr = dsbuf->SetVolume(vol);
	if (FAILED (hr))
		write_log (_T("CDA: SetVolume(%d) failed: %s\n"), vol, DXError (hr));
#endif
}
bool cda_audio::play(int bufnum)
{
	if (!active)
		return false;
#if CDADS
	DWORD status;
	HRESULT hr = dsbuf->GetStatus (&status);
	if (FAILED (hr)) {
		write_log (_T("CDA: GetStatus() failed: %s\n"), DXError (hr));
		return false;
	}
	if (status & DSBSTATUS_BUFFERLOST) {
		write_log (_T("CDA: bufferlost\n"));
		return false;
	}
	if ((status & (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING)) != (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING)) {
		dsbuf->SetCurrentPosition((1 - bufnum) * bufsize);
		dsbuf->Play(0, 0, DSBPLAY_LOOPING);
		playing = true;
	}
	PVOID ptr;
	DWORD len;
	if (SUCCEEDED(dsbuf->Lock(bufnum * bufsize, bufsize, &ptr, &len, NULL, NULL, 0))) {
		memcpy (ptr, buffers[bufnum], bufsize);
		dsbuf->Unlock(ptr, len, NULL, NULL);
	}
	return true;
#else
	uae_s16 *p = (uae_s16*)(buffers[bufnum]);
	if (volume[0] != 32768 || volume[1] != 32768) {
		for (int i = 0; i < num_sectors * sectorsize / 4; i++) {
			p[i * 2 + 0] = p[i * 2 + 0] * volume[0] / 32768;
			p[i * 2 + 1] = p[i * 2 + 1] * volume[1] / 32768;
		}
	}
	MMRESULT mmr = waveOutWrite (wavehandle, &whdr[bufnum], sizeof (WAVEHDR));
	if (mmr != MMSYSERR_NOERROR) {
		write_log (_T("IMAGE CDDA: waveOutWrite %d\n"), mmr);
		return false;
	}
	return true;
#endif
}
void cda_audio::wait(int bufnum)
{
	if (!active || !playing)
		return;
#if CDADS
	WaitForSingleObject (notifyevent[bufnum], INFINITE);
	ResetEvent (notifyevent[bufnum]);
#else
	while (!(whdr[bufnum].dwFlags & WHDR_DONE))
		Sleep (10);
#endif
}

bool cda_audio::isplaying(int bufnum)
{
	if (!active || !playing)
		return false;
	return (whdr[bufnum].dwFlags & WHDR_DONE) == 0;
}


static uae_sem_t play_sem;

static void sub_deinterleave(const uae_u8 *s, uae_u8 *d)
{
	for (int i = 0; i < 8 * 12; i++) {
		int dmask = 0x80;
		int smask = 1 << (7 - (i / 12));
		(*d) = 0;
		for (int j = 0; j < 8; j++) {
			(*d) |= (s[(i % 12) * 8 + j] & smask) ? dmask : 0;
			dmask >>= 1;
		}
		d++;
	}
}

static void sub_to_deinterleaved(const uae_u8 *s, uae_u8 *d)
{
	for (int i = 0; i < 8 * 12; i++) {
		int dmask = 0x80;
		int smask = 1 << (7 - (i / 12));
		(*d) = 0;
		for (int j = 0; j < 8; j++) {
			(*d) |= (s[(i % 12) * 8 + j] & smask) ? dmask : 0;
			dmask >>= 1;
		}
		d++;
	}
}

static void cdda_closewav(struct cda_play *ciw)
{
	if (ciw->cdda_wavehandle != NULL)
		waveOutClose(ciw->cdda_wavehandle);
	ciw->cdda_wavehandle = NULL;
}

// DAE CDDA based on Larry Osterman's "Playing Audio CDs" blog series

static int cdda_openwav(struct cda_play *ciw)
{
	WAVEFORMATEX wav = { 0 };
	MMRESULT mmr;

	wav.cbSize = 0;
	wav.nChannels = 2;
	wav.nSamplesPerSec = 44100;
	wav.wBitsPerSample = 16;
	wav.nBlockAlign = wav.wBitsPerSample / 8 * wav.nChannels;
	wav.nAvgBytesPerSec = wav.nBlockAlign * wav.nSamplesPerSec;
	wav.wFormatTag = WAVE_FORMAT_PCM;
	mmr = waveOutOpen(&ciw->cdda_wavehandle, WAVE_MAPPER, &wav, 0, 0, WAVE_ALLOWSYNC | WAVE_FORMAT_DIRECT);
	if (mmr != MMSYSERR_NOERROR) {
		write_log(_T("IOCTL CDDA: wave open %d\n"), mmr);
		cdda_closewav(ciw);
		return 0;
	}
	return 1;
}

int ciw_cdda_setstate(struct cda_play *ciw, int state, int playpos)
{
	ciw->cdda_play_state = state;
	if (ciw->cdda_statusfunc)
		return ciw->cdda_statusfunc(ciw->cdda_play_state, playpos);
	return 0;
}

void ioctl_next_cd_audio_buffer_callback(int bufnum, void *param)
{
	struct cda_play *ciw = (struct cda_play*)param;
	uae_sem_wait(&play_sem);
	if (bufnum >= 0) {
		ciw->cda_bufon[bufnum] = 0;
		bufnum = 1 - bufnum;
		if (ciw->cda_bufon[bufnum])
			audio_cda_new_buffer(&ciw->cas, (uae_s16 *)ciw->cda->buffers[bufnum], CDDA_BUFFERS * 2352 / 4, bufnum, ioctl_next_cd_audio_buffer_callback, ciw);
		else
			bufnum = -1;
	}
	if (bufnum < 0) {
		audio_cda_new_buffer(&ciw->cas, NULL, 0, -1, NULL, ciw);
	}
	uae_sem_post(&play_sem);
}

static bool cdda_play2(struct cda_play *ciw, int *outpos)
{
	int cdda_pos = ciw->cdda_start;
	int bufnum;
	int buffered;
	int i;
	int oldplay;
	int idleframes;
	int muteframes;
	int readblocksize = 2352 + 96;
	int mode = currprefs.sound_cdaudio;
	bool restart = false;

	while (ciw->cdda_play == 0)
		sleep_millis(10);
	oldplay = -1;

	ciw->cda_bufon[0] = ciw->cda_bufon[1] = 0;
	bufnum = 0;
	buffered = 0;

	memset(&ciw->cas, 0, sizeof(struct cd_audio_state));
	ciw->cda = new cda_audio(CDDA_BUFFERS, 2352, 44100, mode != 0);

	while (ciw->cdda_play > 0) {

		if (mode) {
			while (ciw->cda_bufon[bufnum] && ciw->cdda_play > 0) {
				if (cd_audio_mode_changed) {
					restart = true;
					goto end;
				}
				sleep_millis(10);
			}
		} else {
			ciw->cda->wait(bufnum);
		}
		if (ciw->cdda_play <= 0)
			goto end;
		ciw->cda_bufon[bufnum] = 0;

		if (oldplay != ciw->cdda_play) {
			idleframes = 0;
			muteframes = 0;
			bool seensub = false;
			struct _timeb tb1, tb2;
			_ftime(&tb1);
			cdda_pos = ciw->cdda_start;
			ciw->cd_last_pos = cdda_pos;
			oldplay = ciw->cdda_play;
			write_log(_T("CDDA: playing from %d to %d\n"), ciw->cdda_start, ciw->cdda_end);
			ciw->subcodevalid = false;
			idleframes = ciw->cdda_delay_frames;
			while (ciw->cdda_paused && ciw->cdda_play > 0) {
				sleep_millis(10);
				idleframes = -1;
			}
			// force spin up
			if (isaudiotrack(&ciw->di->toc, cdda_pos))
				ciw->read_block(ciw, ciw->unitnum, ciw->cda->buffers[bufnum], cdda_pos, CDDA_BUFFERS, readblocksize);
			if (!isaudiotrack(&ciw->di->toc, cdda_pos - 150))
				muteframes = 75;

			if (ciw->cdda_scan == 0) {
				// find possible P-subchannel=1 and fudge starting point so that
				// buggy CD32/CDTV software CD+G handling does not miss any frames
				bool seenindex = false;
				for (int sector = cdda_pos - 200; sector < cdda_pos; sector++) {
					uae_u8 *dst = ciw->cda->buffers[bufnum];
					if (sector >= 0 && isaudiotrack(&ciw->di->toc, sector) && ciw->read_block(ciw, ciw->unitnum, dst, sector, 1, readblocksize) > 0) {
						uae_u8 subbuf[SUB_CHANNEL_SIZE];
						sub_deinterleave(dst + 2352, subbuf);
						if (seenindex) {
							for (int i = 2 * SUB_ENTRY_SIZE; i < SUB_CHANNEL_SIZE; i++) {
								if (subbuf[i]) { // non-zero R-W subchannels?
									int diff = cdda_pos - sector + 2;
									write_log(_T("-> CD+G start pos fudge -> %d (%d)\n"), sector, -diff);
									idleframes -= diff;
									cdda_pos = sector;
									seensub = true;
									break;
								}
							}
						} else if (subbuf[0] == 0xff) { // P == 1?
							seenindex = true;
						}
					}
				}
			}
			cdda_pos -= idleframes;

			if (*outpos < 0) {
				_ftime(&tb2);
				int diff = (int)((tb2.time * (uae_s64)1000 + tb2.millitm) - (tb1.time * (uae_s64)1000 + tb1.millitm));
				diff -= ciw->cdda_delay;
				if (idleframes >= 0 && diff < 0 && ciw->cdda_play > 0)
					sleep_millis(-diff);
				if (diff > 0 && !seensub) {
					int ch = diff / 7 + 25;
					if (ch > idleframes)
						ch = idleframes;
					idleframes -= ch;
					cdda_pos += ch;
				}
				ciw_cdda_setstate(ciw, AUDIO_STATUS_IN_PROGRESS, cdda_pos);
			}
		}

		if ((cdda_pos < ciw->cdda_end || ciw->cdda_end == 0xffffffff) && !ciw->cdda_paused && ciw->cdda_play) {

			if (idleframes <= 0 && cdda_pos >= ciw->cdda_start && !isaudiotrack(&ciw->di->toc, cdda_pos)) {
				ciw_cdda_setstate(ciw, AUDIO_STATUS_PLAY_ERROR, -1);
				write_log(_T("IOCTL: attempted to play data track %d\n"), cdda_pos);
				goto end; // data track?
			}

			gui_flicker_led(LED_CD, ciw->unitnum - 1, LED_CD_AUDIO);

			uae_sem_wait(&ciw->sub_sem);

			ciw->subcodevalid = false;
			memset(ciw->subcode, 0, sizeof ciw->subcode);
			memset(ciw->cda->buffers[bufnum], 0, CDDA_BUFFERS * readblocksize);

			if (cdda_pos >= 0) {

				ciw_cdda_setstate(ciw, AUDIO_STATUS_IN_PROGRESS, cdda_pos);

				int r = ciw->read_block(ciw, ciw->unitnum, ciw->cda->buffers[bufnum], cdda_pos, CDDA_BUFFERS, readblocksize);
				if (r < 0) {
					ciw_cdda_setstate(ciw, AUDIO_STATUS_PLAY_ERROR, -1);
					uae_sem_post(&ciw->sub_sem);
					goto end;
				}
				if (r > 0) {
					for (i = 0; i < CDDA_BUFFERS; i++) {
						memcpy(ciw->subcode + i * SUB_CHANNEL_SIZE, ciw->cda->buffers[bufnum] + readblocksize * i + 2352, SUB_CHANNEL_SIZE);
					}
					for (i = 1; i < CDDA_BUFFERS; i++) {
						memmove(ciw->cda->buffers[bufnum] + 2352 * i, ciw->cda->buffers[bufnum] + readblocksize * i, 2352);
					}
					ciw->subcodevalid = true;
				}
			}

			for (i = 0; i < CDDA_BUFFERS; i++) {
				if (muteframes > 0) {
					memset(ciw->cda->buffers[bufnum] + 2352 * i, 0, 2352);
					muteframes--;
				}
				if (idleframes > 0) {
					idleframes--;
					memset(ciw->cda->buffers[bufnum] + 2352 * i, 0, 2352);
					memset(ciw->subcode + i * SUB_CHANNEL_SIZE, 0, SUB_CHANNEL_SIZE);
				} else if (cdda_pos < ciw->cdda_start && ciw->cdda_scan == 0) {
					memset(ciw->cda->buffers[bufnum] + 2352 * i, 0, 2352);
				}
			}
			if (idleframes > 0)
				ciw->subcodevalid = false;

			if (ciw->cdda_subfunc)
				ciw->cdda_subfunc(ciw->subcode, CDDA_BUFFERS);

			uae_sem_post(&ciw->sub_sem);

			if (ciw->subcodevalid) {
				uae_sem_wait(&ciw->sub_sem2);
				memcpy(ciw->subcodebuf, ciw->subcode + (CDDA_BUFFERS - 1) * SUB_CHANNEL_SIZE, SUB_CHANNEL_SIZE);
				uae_sem_post(&ciw->sub_sem2);
			}

			if (mode) {
				if (ciw->cda_bufon[0] == 0 && ciw->cda_bufon[1] == 0) {
					ciw->cda_bufon[bufnum] = 1;
					ioctl_next_cd_audio_buffer_callback(1 - bufnum, ciw);
				}
				audio_cda_volume(&ciw->cas, ciw->cdda_volume[0], ciw->cdda_volume[1]);
				ciw->cda_bufon[bufnum] = 1;
			} else {
				ciw->cda_bufon[bufnum] = 1;
				ciw->cda->setvolume(ciw->cdda_volume[0], ciw->cdda_volume[1]);
				if (!ciw->cda->play(bufnum)) {
					ciw_cdda_setstate(ciw, AUDIO_STATUS_PLAY_ERROR, -1);
					goto end; // data track?
				}
			}

			if (ciw->cdda_scan) {
				cdda_pos += ciw->cdda_scan * CDDA_BUFFERS;
				if (cdda_pos < 0)
					cdda_pos = 0;
			} else {
				if (cdda_pos < 0 && cdda_pos + CDDA_BUFFERS >= 0)
					cdda_pos = 0;
				else
					cdda_pos += CDDA_BUFFERS;
			}

			if (idleframes <= 0) {
				if (cdda_pos - CDDA_BUFFERS < ciw->cdda_end && cdda_pos >= ciw->cdda_end) {
					cdda_pos = ciw->cdda_end;
					ciw_cdda_setstate(ciw, AUDIO_STATUS_PLAY_COMPLETE, cdda_pos);
					ciw->cdda_play_finished = 1;
					ciw->cdda_play = -1;
				}
				ciw->cd_last_pos = cdda_pos;
			}
		}

		if (ciw->cda_bufon[0] == 0 && ciw->cda_bufon[1] == 0) {
			while (ciw->cdda_paused && ciw->cdda_play == oldplay)
				sleep_millis(10);
		}

		if (cd_audio_mode_changed) {
			restart = true;
			goto end;
		}

		bufnum = 1 - bufnum;

	}

end:
	*outpos = cdda_pos;
	if (mode) {
		ioctl_next_cd_audio_buffer_callback(-1, ciw);
		if (restart)
			audio_cda_new_buffer(&ciw->cas, NULL, -1, -1, NULL, ciw);
	} else {
		ciw->cda->wait(0);
		ciw->cda->wait(1);
	}

	ciw->subcodevalid = false;
	cd_audio_mode_changed = false;
	delete ciw->cda;

	write_log(_T("CDDA: thread killed\n"));
	return restart;
}

void ciw_cdda_play(void *v)
{
	struct cda_play *ciw = (struct cda_play *)v;
	int outpos = -1;
	cd_audio_mode_changed = false;
	for (;;) {
		if (!cdda_play2(ciw, &outpos)) {
			break;
		}
		ciw->cdda_start = outpos;
		if (ciw->cdda_start + 150 >= ciw->cdda_end) {
			if (ciw->cdda_play >= 0)
				ciw_cdda_setstate(ciw, AUDIO_STATUS_PLAY_COMPLETE, ciw->cdda_end + 1);
			ciw->cdda_play = -1;
			break;
		}
		ciw->cdda_play = 1;
	}
	ciw->cdda_play = 0;
}

void ciw_cdda_stop(struct cda_play *ciw)
{
	if (ciw->cdda_play != 0) {
		ciw->cdda_play = -1;
		while (ciw->cdda_play) {
			sleep_millis(10);
		}
	}
	ciw->cdda_play_finished = 0;
	ciw->cdda_paused = 0;
	ciw->subcodevalid = 0;
}
