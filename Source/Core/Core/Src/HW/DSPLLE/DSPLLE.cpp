// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/


#include "Common.h"
#include "CommonPaths.h"
#include "Atomic.h"
#include "CommonTypes.h"
#include "LogManager.h"
#include "Thread.h"
#include "ChunkFile.h"
#include "IniFile.h"

#include "DSPLLEGlobals.h" // Local
#include "DSPInterpreter.h"
#include "DSPHWInterface.h"
#include "disassemble.h"
#include "DSPSymbols.h"

#include "AudioCommon.h"
#include "Mixer.h"

#include "DSPTables.h"
#include "DSPCore.h"
#include "DSPLLE.h"
#include "../Memmap.h"
#include "../AudioInterface.h"


DSPLLE::DSPLLE() {
	soundStream = NULL;
	m_InitMixer = false;
	m_bIsRunning = false;
	m_cycle_count = 0;
}

DSPLLE::~DSPLLE() {
}

void DSPLLE::DoState(PointerWrap &p)
{
	p.Do(m_InitMixer);

	p.Do(g_dsp.r);
	p.Do(g_dsp.pc);
#if PROFILE
	p.Do(g_dsp.err_pc);
#endif
	p.Do(g_dsp.cr);
	p.Do(g_dsp.reg_stack_ptr);
	p.Do(g_dsp.exceptions);
	p.Do(g_dsp.external_interrupt_waiting);
	for (int i = 0; i < 4; i++) {
		p.Do(g_dsp.reg_stack[i]);
	}
	p.Do(g_dsp.iram_crc);
	p.Do(g_dsp.step_counter);
	p.Do(g_dsp.ifx_regs);
	p.Do(g_dsp.mbox[0]);
	p.Do(g_dsp.mbox[1]);
	UnWriteProtectMemory(g_dsp.iram, DSP_IRAM_BYTE_SIZE, false);
	p.DoArray(g_dsp.iram, DSP_IRAM_SIZE);
	WriteProtectMemory(g_dsp.iram, DSP_IRAM_BYTE_SIZE, false);
	p.DoArray(g_dsp.dram, DSP_DRAM_SIZE);
	p.Do(cyclesLeft);
	p.Do(m_cycle_count);
}

void DSPLLE::EmuStateChange(PLUGIN_EMUSTATE newState)
{
	DSP_ClearAudioBuffer((newState == PLUGIN_EMUSTATE_PLAY) ? false : true);
}

	/* ECTORTODO
void *DllDebugger(void *_hParent, bool Show)
{
#if defined(HAVE_WX) && HAVE_WX
	m_DebuggerFrame = new DSPDebuggerLLE((wxWindow *)_hParent);
	return (void *)m_DebuggerFrame;
#else
	return NULL;
#endif
}
	*/


// Regular thread
void DSPLLE::dsp_thread(DSPLLE *lpParameter)
{
	DSPLLE *dsp_lle = (DSPLLE *)lpParameter;
	while (dsp_lle->m_bIsRunning)
	{
		int cycles = (int)dsp_lle->m_cycle_count;
		if (cycles > 0) {
			if (dspjit) 
				DSPCore_RunCycles(cycles);
			else
				DSPInterpreter::RunCycles(cycles);

			Common::AtomicAdd(dsp_lle->m_cycle_count, -cycles);
		}
		// yield?
	}
}

/* ECTORTODO
void DSPLLE::DSP_DebugBreak()
{
#if defined(HAVE_WX) && HAVE_WX
	// if (m_DebuggerFrame)
	//  	m_DebuggerFrame->DebugBreak();
#endif
}
*/

void DSPLLE::Initialize(void *hWnd, bool bWii, bool bDSPThread)
{
	m_hWnd = hWnd;
	m_bWii = bWii;
	m_bDSPThread = bDSPThread;
	m_InitMixer = false;
	bool bCanWork = true;
	char irom_file[MAX_PATH];
	char coef_file[MAX_PATH];

	snprintf(irom_file, MAX_PATH, "%s%s",
		File::GetSysDirectory().c_str(), GC_SYS_DIR DIR_SEP DSP_IROM);
	if (!File::Exists(irom_file))
		snprintf(irom_file, MAX_PATH, "%s%s",
			File::GetUserPath(D_GCUSER_IDX), DIR_SEP DSP_IROM);
	snprintf(coef_file, MAX_PATH, "%s%s",
		File::GetSysDirectory().c_str(), GC_SYS_DIR DIR_SEP DSP_COEF);
	if (!File::Exists(coef_file))
		snprintf(coef_file, MAX_PATH, "%s%s",
			File::GetUserPath(D_GCUSER_IDX), DIR_SEP DSP_COEF);
	bCanWork = DSPCore_Init(irom_file, coef_file, AudioCommon::UseJIT());

	g_dsp.cpu_ram = Memory::GetPointer(0);
	DSPCore_Reset();

	if (!bCanWork)
	{
		DSPCore_Shutdown();
		// No way to shutdown Core from here? Hardcore shutdown!
		exit(EXIT_FAILURE);
		return;
	}

	m_bIsRunning = true;

	InitInstructionTable();

	if (m_bDSPThread)
	{
//		m_hDSPThread = new Common::Thread(dsp_thread, (void *)this);
		m_hDSPThread = std::thread(dsp_thread, this);
	}
/*
ECTORTODO
#if defined(HAVE_WX) && HAVE_WX
	if (m_DebuggerFrame)
		m_DebuggerFrame->Refresh();
#endif
		*/
}

void DSPLLE::DSP_StopSoundStream()
{
	DSPInterpreter::Stop();
	m_bIsRunning = false;
	if (m_bDSPThread)
	{
		m_hDSPThread.join();
	}
}

void DSPLLE::Shutdown()
{
	AudioCommon::ShutdownSoundStream();
	DSPCore_Shutdown();
}

u16 DSPLLE::DSP_WriteControlRegister(u16 _uFlag)
{
	UDSPControl Temp(_uFlag);
	if (!m_InitMixer)
	{
		if (!Temp.DSPHalt && Temp.DSPInit)
		{
			unsigned int AISampleRate, DACSampleRate;
			AudioInterface::Callback_GetSampleRate(AISampleRate, DACSampleRate);
			soundStream = AudioCommon::InitSoundStream(new CMixer(AISampleRate, DACSampleRate), m_hWnd); 
			if(!soundStream) PanicAlert("Error starting up sound stream");
			// Mixer is initialized
			m_InitMixer = true;
		}
	}
	DSPInterpreter::WriteCR(_uFlag);

	// Check if the CPU has set an external interrupt (CR_EXTERNAL_INT)
	// and immediately process it, if it has.
	if (_uFlag & 2)
	{
		if (!m_bDSPThread)
		{
			DSPCore_CheckExternalInterrupt();
			DSPCore_CheckExceptions();
		}
		else
		{
			DSPCore_SetExternalInterrupt(true);
		}

	}

	return DSPInterpreter::ReadCR();
}

u16 DSPLLE::DSP_ReadControlRegister()
{
	return DSPInterpreter::ReadCR();
}

u16 DSPLLE::DSP_ReadMailBoxHigh(bool _CPUMailbox)
{
	if (_CPUMailbox)
		return gdsp_mbox_read_h(GDSP_MBOX_CPU);
	else
		return gdsp_mbox_read_h(GDSP_MBOX_DSP);
}

u16 DSPLLE::DSP_ReadMailBoxLow(bool _CPUMailbox)
{
	if (_CPUMailbox)
		return gdsp_mbox_read_l(GDSP_MBOX_CPU);
	else
		return gdsp_mbox_read_l(GDSP_MBOX_DSP);
}

void DSPLLE::DSP_WriteMailBoxHigh(bool _CPUMailbox, u16 _uHighMail)
{
	if (_CPUMailbox)
	{
		if (gdsp_mbox_peek(GDSP_MBOX_CPU) & 0x80000000)
		{
			ERROR_LOG(DSPLLE, "Mailbox isnt empty ... strange");
		}

#if PROFILE
		if ((_uHighMail) == 0xBABE)
		{
			ProfilerStart();
		}
#endif

		gdsp_mbox_write_h(GDSP_MBOX_CPU, _uHighMail);
	}
	else
	{
		ERROR_LOG(DSPLLE, "CPU cant write to DSP mailbox");
	}
}

void DSPLLE::DSP_WriteMailBoxLow(bool _CPUMailbox, u16 _uLowMail)
{
	if (_CPUMailbox)
	{
		gdsp_mbox_write_l(GDSP_MBOX_CPU, _uLowMail);
	}
	else
	{
		ERROR_LOG(DSPLLE, "CPU cant write to DSP mailbox");
	}
}

void DSPLLE::DSP_Update(int cycles)
{
	unsigned int dsp_cycles = cycles / 6;  //(jit?20:6);

// Sound stream update job has been handled by AudioDMA routine, which is more efficient
/*
	// This gets called VERY OFTEN. The soundstream update might be expensive so only do it 200 times per second or something.
	int cycles_between_ss_update;

	if (g_dspInitialize.bWii)
		cycles_between_ss_update = 121500000 / 200;
	else
		cycles_between_ss_update = 81000000 / 200;
	
	m_cycle_count += cycles;
	if (m_cycle_count > cycles_between_ss_update)
	{
		while (m_cycle_count > cycles_between_ss_update)
			m_cycle_count -= cycles_between_ss_update;
		soundStream->Update();
	}
*/
	// If we're not on a thread, run cycles here.
	if (!m_bDSPThread)
	{
		// ~1/6th as many cycles as the period PPC-side.
		DSPCore_RunCycles(dsp_cycles);
	}
	else
	{
		// Wait for dsp thread to catch up reasonably. Note: this logic should be thought through.
		while (m_cycle_count > dsp_cycles)
			;
		Common::AtomicAdd(m_cycle_count, dsp_cycles);
	}
}

void DSPLLE::DSP_SendAIBuffer(unsigned int address, unsigned int num_samples)
{
	if (!soundStream)
		return;

	CMixer *pMixer = soundStream->GetMixer();

	if (pMixer != 0 && address != 0)
	{
		const short *samples = (const short *)&g_dsp.cpu_ram[address & Memory::RAM_MASK];
		pMixer->PushSamples(samples, num_samples);
	}

	soundStream->Update();
}

void DSPLLE::DSP_ClearAudioBuffer(bool mute)
{
	if (soundStream)
		soundStream->Clear(mute);
}

#define LLE_CONFIG_FILE "DSPLLE.ini"

void DSPLLE_LoadConfig()
{
	// first load defaults
	IniFile file;
	file.Load((std::string(File::GetUserPath(D_CONFIG_IDX)) + LLE_CONFIG_FILE).c_str());
	ac_Config.Load(file);
}

void DSPLLE_SaveConfig()
{
	IniFile file;
	file.Load((std::string(File::GetUserPath(D_CONFIG_IDX)) + LLE_CONFIG_FILE).c_str());
	ac_Config.Set(file);
	file.Save((std::string(File::GetUserPath(D_CONFIG_IDX)) + LLE_CONFIG_FILE).c_str());
}