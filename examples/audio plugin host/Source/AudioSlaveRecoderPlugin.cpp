#include "AudioSlaveRecorderPlugin.h"
#include "PluginSharedData.h"

extern SharedResourcePointer<AudioPlayerPluginSharedData> sharedData;

AudioSlaveRecorderPlugin::AudioSlaveRecorderPlugin() : writeThread("AUDIO_RECORDER"), state(TransportState::NoFile)
{
    BusesLayout layout;
    layout.inputBuses.add(AudioChannelSet::stereo());
    layout.outputBuses.add(AudioChannelSet::disabled());
    setBusesLayout(layout);

	formatManager.registerBasicFormats();
    writeThread.startThread();

    // register at shared data for control from AudioPlayerPlugin
    sharedData->setRecorder(this);
}

AudioSlaveRecorderPlugin::~AudioSlaveRecorderPlugin()
{
    if (state >= Stopped) {
        changeState(Unloading);
        while (state != NoFile);
    }
    audioCallbacks.clear();
    sharedData->recorderKilled();
}

void AudioSlaveRecorderPlugin::processBlock(AudioSampleBuffer& buffer, MidiBuffer&)
{
    // handle callbacks
    for (const auto &callback : audioCallbacks) {
        if (callback != nullptr) {
            callback->audioDeviceIOCallback(buffer.getArrayOfReadPointers(), buffer.getNumChannels(), nullptr, 0, buffer.getNumSamples());
        }
    }
    
    // write audio data, if in recording mode
    if (state == TransportState::Recording && activeWriter) {
		const ScopedLock sl(stateLock);
		activeWriter->write(buffer.getArrayOfReadPointers(), buffer.getNumSamples());

	}
}

void AudioSlaveRecorderPlugin::fillInPluginDescription(PluginDescription & d) const
{
	d.name = getName();
	d.uid = d.name.hashCode();
	d.category = "Output devices";
	d.pluginFormatName = "Internal";
	d.manufacturerName = "youdisc.com";
	d.version = "1.0";
	d.isInstrument = false;
	d.numInputChannels = getNumInputChannels();
	d.numOutputChannels = getNumOutputChannels();
}

void AudioSlaveRecorderPlugin::getStateInformation(MemoryBlock& data) {
    // save filename
	if (&data && filename)
		MemoryOutputStream(data, true).writeString(*filename);
}

void AudioSlaveRecorderPlugin::setStateInformation(const void* data, int sizeInBytes) {
    // restore filename (open is handled later from the AudioPLayerPlugin as needed)
	if (data) {
		setFilename(new String(MemoryInputStream(data, sizeInBytes, false).readString()));
	}
}

/**
    add an audio callback which is called in the audio loop, e.g. for audio thumbnails
    @param callback the callback to add
*/
void AudioSlaveRecorderPlugin::addAudioCallback(AudioIODeviceCallback *callback) {
    audioCallbacks.add(callback);
}

/**
    remove a certain audio callback
    @param callback the callback to remove
*/
void AudioSlaveRecorderPlugin::removeAudioCallback(AudioIODeviceCallback *callback) {
    audioCallbacks.removeFirstMatchingValue(callback);
}

/** 
    Change the state of the recorder
    Convention: Stopped state is used for file opened and ready to record.

    Legal new states (not checked!) are
    - Undefined (error or no filename)
    - NoFile (file not loaded)
    - Unloading (ends up in NoFile = file unloaded)
    - Stopping (ends up in Stopped = file opened and ready)
    - Pausing (ends up in Paused = file still open)
    - Starting (ends up in Recording)

    After calling this, new state will immediately be reflected respectively.

    @param newState the new state to switch to
    @returns true, if an error appeared (e.g. no file, file open error)
*/
bool AudioSlaveRecorderPlugin::changeState(TransportState newState)
{
	bool error = false;

    // state changed?
	if (newState != state) {
        // should open the file
        if (state == TransportState::NoFile && newState > TransportState::Unloading) {
			if (!openFileWriter()) {
				// error opening file -> back to undefined
				error = true;
			}
            newState = TransportState::Stopped;
		}

        // check if file must be open already - could have been done here above!
		if (state < TransportState::Stopped && newState > TransportState::Stopped) {
			// no open file, not possible to record
			error = true;
		}

        // change state in locked block
		{
			const ScopedLock sl(stateLock);

            state = newState;
            // delete writer pointer only when file is unloaded
            if (error || newState <= TransportState::Unloading) {
                activeWriter = nullptr;
            }
		}

		// delete writer object w/o lock, could take time to flush to disk
        if (error || newState <= TransportState::Unloading)
            threadedWriter = nullptr;

        // propagate to next state for temporary states and errors
        {
            const ScopedLock sl(stateLock);

            if (newState == TransportState::Stopping)
                state = TransportState::Stopped;

            if (newState == TransportState::Pausing)
                state = TransportState::Paused;

            if (newState == TransportState::Starting)
                state = TransportState::Recording;

            if (error || newState == TransportState::Unloading)
                state = TransportState::NoFile;

        }
	}
	return !error;
}

/**
    Open the currently selected filename for writing.
    @returns true, if the file was successfully opened
*/
bool AudioSlaveRecorderPlugin::openFileWriter()
{
    if (!filename || filename->isEmpty()) {
        Logger::outputDebugString("no file name to open!");
        return false;
    }

    // for recording, existing file must be deleted
    File file = File(*filename);
    if (!file.deleteFile()) {
        Logger::outputDebugString("error deleting existing file");
        return false;
    }

    // open file for writing
    Logger::outputDebugString(String("opening file for writing: ") += *filename);
    ScopedPointer<FileOutputStream> stream = file.createOutputStream();
	if (!stream) {
        Logger::outputDebugString("error opening file!");
        return false;
    }

    // find file format and create writer
    int index = filename->lastIndexOfChar('.');
    if (index == -1) {
        Logger::outputDebugString("no file extension found!");
        return false;
    }
    String extension = filename->substring(index);
    if (extension.isEmpty()) {
        Logger::outputDebugString("no file extension found!");
        return false;
    }
    AudioFormat* format = formatManager.findFormatForFileExtension(extension);
    if (extension.isEmpty()) {
        Logger::outputDebugString("unknown file format/extension!");
        return false;
    }
    AudioFormatWriter* writer = format->createWriterFor(stream, getSampleRate(), getNumInputChannels(), 16, StringPairArray(), 0);
    if (!writer) {
        Logger::outputDebugString("writer could not be created!");
        return false;
    }

    // create threaded writer and set it active
    stream.release();
	threadedWriter = new AudioFormatWriter::ThreadedWriter(writer, writeThread, WRITE_BUFFER);
	const ScopedLock sl(stateLock);
	activeWriter = threadedWriter;

	return true;
}

bool AudioSlaveRecorderPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // accept fixed channels only: stereo in, no out
    return (layouts.getMainInputChannels() == 2
        && layouts.getMainOutputChannels() == 0);
}