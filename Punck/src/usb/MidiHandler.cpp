#include <configManager.h>
#include "USB.h"
#include "MidiHandler.h"
#include "VoiceManager.h"
#include "sequencer.h"
#include "reverb.h"

void MidiHandler::DataIn()
{

}

void MidiHandler::DataOut()
{
	// Handle incoming midi command here
	const uint8_t* outBuffBytes = reinterpret_cast<const uint8_t*>(outBuff);

	if (!partialSysEx && outBuffCount == 4) {
		midiEvent(*outBuff);

	} else if (partialSysEx || (outBuffBytes[1] == 0xF0 && outBuffCount > 3)) {		// Sysex
		// sysEx will be padded when supplied by usb - add only actual sysEx message bytes to array
		uint16_t sysExCnt = partialSysEx ? 1 : 2;		// If continuing a long sysex command only ignore first (size) byte

		uint16_t i;
		for (i = partialSysEx ? sysExCount : 0; i < sysexMaxSize; ++i) {
			if (outBuffBytes[sysExCnt] == 0xF7) {
				partialSysEx = false;
				break;
			}
			if (sysExCnt >= outBuffCount) {				// Long SysEx command will be received in multiple packets
				partialSysEx = true;
				break;
			}

			sysEx[i] = outBuffBytes[sysExCnt++];

			// remove 1 byte padding at the beginning of each 32 bit word
			if (sysExCnt % 4 == 0) {
				++sysExCnt;
			}
		}
		sysExCount = i;
		if (!partialSysEx) {
			ProcessSysex();
		}
	}
}


void MidiHandler::ActivateEP()
{
	EndPointActivate(USB::Midi_In,   Direction::in,  EndPointType::Bulk);
	EndPointActivate(USB::Midi_Out,  Direction::out, EndPointType::Bulk);

	EndPointTransfer(Direction::out, USB::Midi_Out, USB::ep_maxPacket);
}


uint32_t MidiHandler::ConstructSysEx(const uint8_t* dataBuffer, uint32_t dataLen, const uint8_t* headerBuffer, const uint32_t headerLen, const bool noSplit)
{
	// Constructs a Sysex packet: data split into 4 byte words, each starting with appropriate sysex header byte
	// Bytes in sysEx commands must have upper nibble = 0 (ie only 0-127 values) so double length and split bytes into nibbles (unless noSplit specified)

	if (!noSplit) {
		dataLen *= 2;
	}
	uint32_t pos = 0;

	sysExOut[pos++] = 0x04;					// 0x4	SysEx starts or continues
	sysExOut[pos++] = 0xF0;

	// header must be 7 bit values so no need to split
	for (uint32_t i = 0; i < headerLen; ++i) {
		sysExOut[pos++] = headerBuffer[i];
		if (pos % 4 == 0) {
			sysExOut[pos++] = 0x04;			// 0x4	SysEx starts or continues
		}
	}

	// FIXME - this is a specific fix for sending out 2 byte headers; end byte codes should be used correctly as below
	if (dataLen == 0 && pos == 5) {
		sysExOut[pos - 1] = 0x5;
	}

	bool lowerNibble = true;
	for (uint32_t i = 0; i < dataLen; ++i) {
		if (noSplit) {
			sysExOut[pos++] = dataBuffer[i];
		} else {
			if (lowerNibble) {
				sysExOut[pos++] = dataBuffer[i / 2] & 0xF;
			} else {
				sysExOut[pos++] = dataBuffer[i / 2] >> 4;
			}
			lowerNibble = !lowerNibble;
		}

		// add 1 byte padding at the beginning of each 32 bit word to indicate length of remaining message + 0xF7 termination byte
		if (pos % 4 == 0) {
			const uint32_t rem = dataLen - i;

			if (rem == 3) {
				sysExOut[pos++] = 0x07;		// 0x7	SysEx ends with following three bytes.
			} else if (rem == 2) {
				sysExOut[pos++] = 0x06;		// 0x6	SysEx ends with following two bytes.
			} else if (rem == 1) {
				sysExOut[pos++] = 0x05;		// 0x5	Single-byte System Common Message or SysEx ends with following single byte.
			} else {
				sysExOut[pos++] = 0x04;		// 0x4	SysEx starts or continues
			}
		}
	}
	sysExOut[pos++] = 0xF7;

	if (pos > sysexMaxSize) {
		int susp = 1;
	}

	pos = ((pos + 3) / 4) * 4;				// round up output size to multiple of 4
	return pos;
}


uint32_t MidiHandler::ReadCfgSysEx(uint8_t headerLength)
{
	// Converts a Configuration encoded Sysex packet to a regular byte array (two bytes are header)
	// Data split into 4 byte words, each starting with appropriate sysEx header byte
	bool lowerNibble = true;
	for (uint32_t i = headerLength; i < sysExCount; ++i) {
		uint32_t idx = (i - headerLength) / 2;
		if (lowerNibble) {
			configManager.rxBuffer[idx] = sysEx[i];
		} else {
			configManager.rxBuffer[idx] += sysEx[i] << 4;
		}
		lowerNibble = !lowerNibble;
	}
	return (sysExCount - headerLength) / 2;
}


void MidiHandler::ProcessSysex()
{
	// Check if SysEx contains read config command
	switch (sysEx[0]) {
		case GetVoiceConfig:
			if (sysEx[1] < VoiceManager::Voice::count) {
				VoiceManager::Voice voice = (VoiceManager::Voice)sysEx[1];

				// Insert header data
				const uint8_t cfgHeader[2] = {GetVoiceConfig, voice};

				uint8_t* cfgBuffer = nullptr;
				NoteMapper& vm = voiceManager.noteMapper[voice];
				const uint32_t bytes = vm.drumVoice->SerialiseConfig(&cfgBuffer, vm.voiceIndex);
				const uint32_t len = ConstructSysEx(cfgBuffer, bytes, cfgHeader, 2, split);

				usb->SendData(sysExOut, len, inEP);
			}
			break;

		case SetVoiceConfig:
			if (sysEx[1] < VoiceManager::Voice::count) {
				const uint32_t bytes = ReadCfgSysEx(2);
				voiceManager.noteMapper[sysEx[1]].drumVoice->StoreConfig(configManager.rxBuffer, bytes);
				configManager.ScheduleSave();
			}
			break;

		case GetReverbConfig:
		{
			// Insert header data
			const uint8_t cfgHeader = GetReverbConfig;

			uint8_t* cfgBuffer = nullptr;
			const uint32_t bytes = reverb.SerialiseConfig(&cfgBuffer);
			const uint32_t len = ConstructSysEx(cfgBuffer, bytes, &cfgHeader, 1, split);

			usb->SendData(sysExOut, len, inEP);
			break;
		}

		case SetReverbConfig:
		{
			const uint32_t bytes = ReadCfgSysEx(1);
			reverb.StoreConfig(configManager.rxBuffer);
			configManager.ScheduleSave();
			break;
		}

		case GetMixConfig:
		{
			const uint8_t cfgHeader = GetMixConfig;
			uint8_t* cfgBuffer = nullptr;
			const uint32_t bytes = voiceManager.GetMixConfig(&cfgBuffer);
			const uint32_t len = ConstructSysEx(cfgBuffer, bytes, &cfgHeader, 1, split);
			usb->SendData(sysExOut, len, inEP);
			break;
		}

		case SetMixConfig:
		{
			const uint32_t bytes = ReadCfgSysEx(1);
			voiceManager.StoreMixConfig(configManager.rxBuffer, bytes);
			configManager.ScheduleSave();
			break;
		}

		case GetMeter:
		{
			const uint8_t cfgHeader = GetMeter;
			VoiceManager::Meter meter;
			voiceManager.GetMeter(meter);
			const uint32_t len = ConstructSysEx(reinterpret_cast<uint8_t*>(&meter), sizeof(meter), &cfgHeader, 1, split);
			usb->SendData(sysExOut, len, inEP);
			break;
		}

		case StartStopSeq:
			sequencer.StartStop(sysEx[1]);
			break;

		case SaveConfig:
			configManager.RequestSave();				// saved from the main loop (not inside the USB interrupt)
			break;

		case GetStatus:
		{
			// Get playing status from sequence
			configManager.rxBuffer[0] = GetStatus;
			configManager.rxBuffer[1] = sequencer.playing ? 1 : 0;					// playing
			configManager.rxBuffer[2] = sequencer.activeSequence; 					// active sequence
			configManager.rxBuffer[3] = sequencer.currentBar;     					// current bar
			configManager.rxBuffer[4] = sequencer.currentBeat;    					// current beat
			configManager.rxBuffer[5] = i2sUnderrun;		    					// I2S Underrun

			const uint32_t len = ConstructSysEx(configManager.rxBuffer, 6, nullptr, 0, noSplit);
			usb->SendData(sysExOut, len, inEP);
			break;
		}

		case GetSequence:
		{
			uint8_t seq = sysEx[1];							// if passed 127 then requesting currently active sequence
			const uint8_t bar = sysEx[2];
			if (seq == getActiveSequence) {
				seq = sequencer.activeSequence;
			}
			auto seqInfo = sequencer.GetSeqInfo(seq);

			// Insert header data
			configManager.rxBuffer[0] = GetSequence;
			configManager.rxBuffer[1] = seq;					// sequence
			configManager.rxBuffer[2] = seqInfo.beatsPerBar;	// beats per bar
			configManager.rxBuffer[3] = seqInfo.bars;			// bars
			configManager.rxBuffer[4] = bar;					// bar number

			uint8_t* cfgBuffer = nullptr;
			const uint32_t bytes = sequencer.GetBar(&cfgBuffer, seq, bar);
			const uint32_t len = ConstructSysEx(cfgBuffer, bytes, configManager.rxBuffer, 5, noSplit);
			usb->SendData(sysExOut, len, inEP);
			break;
		}

		case SetSequence:
		{
			// Header information
			const uint32_t seq = sysEx[1];					// sequence
			const uint32_t beatsPerBar = sysEx[2];			// beats per bar
			const uint32_t bars = sysEx[3];					// bars
			const uint32_t bar = sysEx[4];					// bar number

			sequencer.StoreConfig(sysEx + 5, sysExCount - 5, seq, bar, beatsPerBar, bars);
			configManager.ScheduleSave();
			break;
		}

		case GetSamples:
		{
			const uint8_t samplePlayer = sysEx[1];
			const uint8_t cfgHeader[2] = {GetSamples, samplePlayer};		// Insert header data

			uint8_t* cfgBuffer = nullptr;
			const uint32_t bytes = voiceManager.samples.SerialiseSampleNames(&cfgBuffer, samplePlayer);
			const uint32_t len = ConstructSysEx(cfgBuffer, bytes, cfgHeader, 2, noSplit);

			usb->SendData(sysExOut, len, inEP);
			break;
		}
	}


}


void MidiHandler::midiEvent(const uint32_t data)
{
	auto midiData = MidiData(data);

	if (midiData.msg == System) {
		if (midiData.chn == 0x8) {								// 0xF8: MIDI clock tick
			voiceManager.MidiClock();
		}
		return;
	}

	switch (midiData.msg) {
	case NoteOff:
		break;

	case NoteOn:
		voiceManager.NoteOn(MidiNote(midiData.db1, midiData.db2), midiData.chn);	// Channel routing happens in VoiceManager
		break;

	case PitchBend:
		pitchBend = static_cast<uint32_t>(midiData.db1) + (midiData.db2 << 7);
		break;
	}
}


void MidiHandler::serialHandler(uint32_t data)
{
	// Byte-wise MIDI parser for the TRS input (called from UART interrupt).
	// Upstream parser had no running status and broke when clock bytes arrived mid-message.
	const uint8_t b = data & 0xFF;

	if (b >= 0xF8) {											// Realtime: can occur between any bytes, does not affect running status
		if (b == 0xF8) {
			voiceManager.MidiClock();
		}
		return;
	}

	if (b & 0x80) {												// Status byte
		serialStatus = (b >= 0xF0) ? 0 : b;						// System common/SysEx cancels running status
		serialCount = 0;
		return;
	}

	if (serialStatus == 0) {									// Data byte without status (eg SysEx payload)
		return;
	}

	serialData[serialCount++] = b;
	const uint8_t type = serialStatus >> 4;
	const uint8_t needed = (type == ProgramChange || type == ChannelPressure) ? 1 : 2;
	if (serialCount >= needed) {
		MidiData event;
		event.data = 0;
		event.chn = serialStatus & 0x0F;
		event.msg = type;
		event.db1 = serialData[0];
		event.db2 = (needed == 2) ? serialData[1] : 0;
		serialCount = 0;										// Keep status for running status
		midiEvent(event.data);
	}
}




void MidiHandler::ClassSetup(usbRequest& req)
{
}


void MidiHandler::ClassSetupData(usbRequest& req, const uint8_t* data)
{
}


/*
Byte 1									|	Byte2		|	Byte 3		|	Byte 4
Cable Number | Code Index Number (CIN)	|	MIDI_0		|	MIDI_1		|	MIDI_2

CIN		MIDI_x Size Description
0x0		1, 2 or 3	Miscellaneous function codes. Reserved for future extensions.
0x1		1, 2 or 3	Cable events. Reserved for future expansion.
0x2		2			Two-byte System Common messages like MTC, SongSelect, etc.
0x3		3			Three-byte System Common messages like SPP, etc.
0x4		3			SysEx starts or continues
0x5		1			Single-byte System Common Message or SysEx ends with following single byte.
0x6		2			SysEx ends with following two bytes.
0x7		3			SysEx ends with following three bytes.
0x8		3			Note-off
0x9		3			Note-on
0xA		3			Poly-KeyPress
0xB		3			Control Change
0xC		2			Program Change
0xD		2			Channel Pressure
0xE		3			PitchBend Change
0xF		1			Single Byte
*/

// Descriptor definition here as requires constants from USB class
const uint8_t MidiHandler::Descriptor[] = {
	// B.3.1 Standard Audio Control standard Interface Descriptor
	0x09,									// length of descriptor in bytes
	USB::InterfaceDescriptor,				// interface descriptor type
	USB::AudioInterface,					// index of this interface
	0x00,									// alternate setting for this interface
	0x00,									// endpoints excl 0: number of endpoint descriptors to follow
	0x01,									// AUDIO
	0x01,									// AUDIO_Control
	0x00,									// bInterfaceProtocol
	USB::AudioClass,						// string index for interface

	// B.3.2 Class-specific AC Interface Descriptor
	0x09,									// length of descriptor in bytes
	USB::ClassSpecificInterfaceDescriptor,	// descriptor type
	0x01,									// header functional descriptor
	0x00, 0x01,								// bcdADC
	0x09, 0x00,								// wTotalLength
	0x01,									// bInCollection
	0x01,									// baInterfaceNr[1]

	// B.4 MIDIStreaming Interface Descriptors
	// B.4.1 Standard MS Interface Descriptor
	0x09,									// bLength
	USB::InterfaceDescriptor,				// bDescriptorType: interface descriptor
	USB::MidiInterface,						// bInterfaceNumber
	0x00,									// bAlternateSetting
	0x02,									// bNumEndpoints
	0x01,									// bInterfaceClass: Audio
	0x03,									// bInterfaceSubClass: MIDIStreaming
	0x00,									// InterfaceProtocol
	USB::AudioClass,						// iInterface: String Descriptor

	// B.4.2 Class-specific MS Interface Descriptor
	0x07,									// length of descriptor in bytes
	USB::ClassSpecificInterfaceDescriptor,	// bDescriptorType: Class Specific Interface Descriptor
	0x01,									// header functional descriptor
	0x0, 0x01,								// bcdADC
	MidiHandler::MidiClassDescSize, 0,					// wTotalLength

	// B.4.3 MIDI IN Jack Descriptor (Embedded)
	0x06,									// bLength
	USB::ClassSpecificInterfaceDescriptor,	// descriptor type
	0x02,									// bDescriptorSubtype: MIDI_IN_JACK
	0x01,									// bJackType: Embedded
	0x01,									// bJackID
	0x00,									// iJack: No String Descriptor

	// Table B4.4 Midi Out Jack Descriptor (Embedded)
	0x09,									// length of descriptor in bytes
	USB::ClassSpecificInterfaceDescriptor,	// descriptor type
	0x03,									// MIDI_OUT_JACK descriptor
	0x01,									// bJackType: Embedded
	0x02,									// bJackID
	0x01,									// No of input pins
	0x01,									// ID of the Entity to which this Pin is connected.
	0x01,									// Output Pin number of the Entity to which this Input Pin is connected.
	0x00,									// iJack

	//B.5.1 Standard Bulk OUT Endpoint Descriptor
	0x09,									// bLength
	USB::EndpointDescriptor,				// bDescriptorType = endpoint
	USB::Midi_Out,							// bEndpointAddress
	USB::Bulk,								// bmAttributes: 2:Bulk
	LOBYTE(USB::ep_maxPacket),				// wMaxPacketSize
	HIBYTE(USB::ep_maxPacket),
	0x00,									// bInterval in ms : ignored for bulk
	0x00,									// bRefresh Unused
	0x00,									// bSyncAddress Unused

	// B.5.2 Class-specific MS Bulk OUT Endpoint Descriptor
	0x05,									// bLength of descriptor in bytes
	0x25,									// bDescriptorType (Audio Endpoint Descriptor)
	0x01,									// bDescriptorSubtype: MS General
	0x01,									// bNumEmbMIDIJack
	0x01,									// baAssocJackID ID of the Embedded MIDI IN Jack.

	//B.6.1 Standard Bulk IN Endpoint Descriptor
	0x09,									// bLength
	USB::EndpointDescriptor,				// bDescriptorType = endpoint
	USB::Midi_In,							// bEndpointAddress IN endpoint number 3
	USB::Bulk,								// bmAttributes: 2: Bulk, 3: Interrupt endpoint
	LOBYTE(USB::ep_maxPacket),				// wMaxPacketSize
	HIBYTE(USB::ep_maxPacket),
	0x00,									// bInterval in ms
	0x00,									// bRefresh
	0x00,									// bSyncAddress

	// B.6.2 Class-specific MS Bulk IN Endpoint Descriptor
	0x05,									// bLength of descriptor in bytes
	0x25,									// bDescriptorType
	0x01,									// bDescriptorSubtype
	0x01,									// bNumEmbMIDIJack
	0x02,									// baAssocJackID ID of the Embedded MIDI OUT Jack

};


uint32_t MidiHandler::GetInterfaceDescriptor(const uint8_t** buffer) {
	*buffer = Descriptor;
	return sizeof(Descriptor);
}
