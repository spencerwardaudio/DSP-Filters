class SessionDSP
{
public:
    enum FilterType
    {
        Type_HighPass2 = 0
    };
    
    static const void calculusCoefficients (float frequency, float Q, float gain,
                                            int type, double sampleRate,
                                            float &b0, float &b1, float &b2, float &a1, float &a2)
    {
        auto Ts = (float)(1.0 / sampleRate);
        auto w0 = (float)(2.0 * double_Pi * frequency);
        
        auto Ts2 = Ts * Ts;
        auto w02 = w0 * w0;
        
        auto a0 = Q * Ts2 * w02 + 2 * Ts * w0 + 4 * Q;
        
        b0 = (4 * Q) / a0;
        b1 = (-8 * Q) / a0;
        b2 = (4 * Q) / a0;
        
        a1 = (2 * Q * Ts2 * w02 - 8 * Q) / a0;
        a2 = (Q * Ts2 * w02 - 2 * Ts * w0 + 4 * Q) / a0;

    }
};


class MainComponent  : public AudioAppComponent, public SessionDSP, public ChangeListener, private Timer, public AsyncUpdater
{
public:
    MainComponent()
    : state (Stopped), thumbnailCache (5), thumbnail (512, formatManager, thumbnailCache)
    {
        
        triggerAsyncUpdate();
        
        addAndMakeVisible(openButton);
        openButton.setButtonText ("Open");
        openButton.onClick = [this] { openButtonClicked(); };
        
        addAndMakeVisible (playButton);
        playButton.setButtonText ("Play");
        playButton.onClick = [this] { playButtonClicked(); };
        playButton.setColour (TextButton::buttonColourId, Colours::green);
        playButton.setEnabled (false);
        
        addAndMakeVisible (&stopButton);
        stopButton.setButtonText ("Stop");
        stopButton.onClick = [this] { stopButtonClicked(); };
        stopButton.setColour (TextButton::buttonColourId, Colours::red);
        stopButton.setEnabled (false);
        
        addAndMakeVisible(filterCutoffGUI);
        filterCutoffGUI.setRange(20.0, 20000.0);
        filterCutoffGUI.setNumDecimalPlacesToDisplay(1);
        filterCutoffGUI.setSkewFactorFromMidPoint(500.0);
        filterCutoffGUI.onValueChange = [this]
                {
                   if (currentSampleRate > 0.0)
                       updateFilterCuttoff();
                };
        
        setSize (400, 220);
        
        formatManager.registerBasicFormats();
        
        transportSource.addChangeListener (this);
        
        setAudioChannels (0, 2);
        startTimer (100);
        
        thumbnail.addChangeListener(this);
    }
    
    ~MainComponent()
    {
        shutdownAudio();
    }
    
    void updateFilterCuttoff()
    {
        auto cyclesPerSample = filterCutoffGUI.getValue();
        currentCutoffFrequency = static_cast<float>(cyclesPerSample);
        newCutoffFrequency = currentCutoffFrequency;
    }
    
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        
        currentSampleRate = sampleRate;
        updateFilterCuttoff();

        reset();
        
        transportSource.prepareToPlay (samplesPerBlockExpected, sampleRate);
        
    }
    
    void getNextAudioBlock (const AudioSourceChannelInfo& bufferToFill) override
    {
        if (readerSource.get() == nullptr)
            bufferToFill.clearActiveBufferRegion();
        else
            transportSource.getNextAudioBlock (bufferToFill);
        
        //for filtering (session component info)
        SessionDSP::calculusCoefficients ((newCutoffFrequency), (0.75f),
                                          (0.1f),
                                          1 - 1, 44100.0,
                                          theCoefficients.b0, theCoefficients.b1, theCoefficients.b2,
                                          theCoefficients.a1, theCoefficients.a2);
        
        ScopedNoDenormals noDenormals;
        
        auto startSample = bufferToFill.startSample;
        auto numSamples = bufferToFill.numSamples;
        auto numChannels = jmax (2, bufferToFill.buffer->getNumChannels());
        
        jassert (numSamples > 0);
        jassert (numChannels > 0);
        
        for (auto channel = 0; channel < numChannels; channel++)
        {
            auto *samples = bufferToFill.buffer->getWritePointer (channel);
            
            auto b0 = theCoefficients.b0;
            auto b1 = theCoefficients.b1;
            auto b2 = theCoefficients.b2;
            auto a1 = theCoefficients.a1;
            auto a2 = theCoefficients.a2;
            
            auto v1 = theFilters[channel].v1;
            auto v2 = theFilters[channel].v2;
            
            for (auto i = 0; i < numSamples; i++)
            {
                auto input = samples[i + startSample];
                
                auto output = b0 * input + v1;
                v1 = b1 * input + v2 - a1 * output;
                v2 = b2 * input - a2 * output;
                
                samples[i + startSample] = output;
            }
            
            theFilters[channel].v1 = v1;
            theFilters[channel].v2 = v2;
        }
    }
    
    void releaseResources() override
    {
        reset();
        transportSource.releaseResources();
    }
    
    void reset()
    {
        for (auto channel = 0; channel < 2; channel++)
        {
            theFilters[channel].v1 = 0;
            theFilters[channel].v2 = 0;
        }
    }
    
    void resized() override
    {
        openButton.setBounds (10, 10, getWidth() - 280, 20);
        playButton.setBounds (140, 10, getWidth() - 280, 20);
        stopButton.setBounds (270, 10, getWidth() - 280, 20);
        
        filterCutoffGUI.setBounds(10, 40, getWidth() - 20, 20);
        
        Rectangle<int> thumbnailBounds (10, 70, getWidth() - 20, getHeight() - 85);
    }
    
    void shutdownAudio()
    {
        transportSource.stop();
        
        audioSourcePlayer.setSource (nullptr);
        transportSource.setSource (nullptr);
        deviceManager.removeAudioCallback (&audioSourcePlayer);
        deviceManager.closeAudioDevice();
    }
    
    //==============================================================================
    void paint (Graphics& g) override
    {
        Rectangle<int> thumbnailBounds (10, 70, getWidth() - 20, getHeight() - 85);
        
        if (thumbnail.getNumChannels() == 0)
            paintIfNoFileLoaded (g, thumbnailBounds);
        else
            paintIfFileLoaded (g, thumbnailBounds);
    }
    
    
    void changeListenerCallback (ChangeBroadcaster* source) override
    {
        if (source == &transportSource)
            transportSourceChanged();
        if (source == &thumbnail)
            thumbnailChanged();
    }
    
    void paintIfNoFileLoaded (Graphics& g, const Rectangle<int>& thumbnailBounds)
    {
        g.setColour (Colours::darkgrey);
        g.fillRect (thumbnailBounds);
        g.setColour (Colours::white);
        g.drawFittedText ("No File Loaded", thumbnailBounds, Justification::centred, 1.0f);
    }
    
    void paintIfFileLoaded (Graphics& g, const Rectangle<int>& thumbnailBounds)
    {
        g.setColour (Colours::white);
        g.fillRect (thumbnailBounds);
        g.setColour (Colours::blue);
        
        auto audioLength (thumbnail.getTotalLength());
        
        thumbnail.drawChannels (g, thumbnailBounds, 0.0, thumbnail.getTotalLength(), 1.0f);
        
        g.setColour (Colours::green);
        
        auto audioPosition (transportSource.getCurrentPosition());
        auto drawPosition ((audioPosition / audioLength) * thumbnailBounds.getWidth() + thumbnailBounds.getX());
        g.drawLine (drawPosition, thumbnailBounds.getY(), drawPosition, thumbnailBounds.getBottom(), 2.0f);
    }
    
    void thumbnailChanged()
    {
        repaint();
    }
    
private:
    //==============================================================================
    enum TransportState
    {
        Stopped,
        Starting,
        Playing,
        Stopping
    };
    
    void changeState (TransportState newState)
    {
        if (state != newState)
        {
            state = newState;
            
            switch (state)
            {
                case Stopped:
                    stopButton.setEnabled (false);
                    playButton.setEnabled (true);
                    transportSource.setPosition (0.0);
                    break;
                    
                case Starting:
                    playButton.setEnabled (false);
                    transportSource.start();
                    break;
                    
                case Playing:
                    stopButton.setEnabled (true);
                    break;
                    
                case Stopping:
                    transportSource.stop();
                    break;
                    
                default:
                    jassertfalse;
                    break;
            }
        }
    }
    
    void transportSourceChanged()
    {
        {
            if (transportSource.isPlaying())
                changeState (Playing);
            else
                changeState (Stopped);
        }
    }
    
    void openButtonClicked()
    {
        FileChooser chooser ("Select a Wave file to play...",
                             {},
                             "*.wav");
        
        if (chooser.browseForFileToOpen())
        {
            File file = chooser.getResult();
            auto* reader = formatManager.createReaderFor (file);
            
            if (reader != nullptr)
            {
                std::unique_ptr<AudioFormatReaderSource> newSource (new AudioFormatReaderSource (reader, true));
                transportSource.setSource (newSource.get(), 0, nullptr, reader->sampleRate);
                playButton.setEnabled (true);
                thumbnail.setSource (new FileInputSource (file));
                readerSource.reset (newSource.release());
            }
        }
    }
    
    void playButtonClicked()
    {
        changeState (Starting);
    }
    
    void stopButtonClicked()
    {
        
        changeState (Stopping);
    }
    
    void timerCallback() override
    {
        repaint();
    }
    
    void handleAsyncUpdate() override
    {
        repaint();
    }
    
    struct FilterCoefficients
    {
        float b0, b1, b2;   // Z transform coefficients (numerator)
        float a1, a2;       // Z transform coefficients (denominator)
    };
    
    struct FilterState
    {
        float v1, v2;       // state variable for TDF2
    };
    
private:
    Slider slider;
    FilterCoefficients theCoefficients;
    FilterState theFilters[2];

    AudioBuffer<float> bufferData;

    TextButton openButton;
    TextButton playButton;
    TextButton stopButton;
    Slider  filterCutoffGUI;
    double currentSampleRate = 0.0;
    float newCutoffFrequency = 0.0f, currentCutoffFrequency = 100.0f;

    AudioDeviceManager deviceManager; //define
    AudioSourcePlayer audioSourcePlayer; //define

    AudioFormatManager formatManager;
    std::unique_ptr<AudioFormatReaderSource> readerSource;
    AudioTransportSource transportSource;
    TransportState state;
    AudioThumbnailCache thumbnailCache;         // statically allocated object
    AudioThumbnail thumbnail;                   // statically allocated object

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
