graph TD
    A["main.cpp<br/>Program Start"] --> B["sdr.init()<br/>Initialize SDR"]
    B --> C["sdr.tune()<br/>Tune to frequency"]
    C --> D["init_buffer()<br/>Create buffer<br/>MultichannelRingbuffer"]
    
    D --> E["sdr.start()"]
    E --> F["Start Reader Thread"]
    E --> G["Main Processing Loop"]
    
    F --> H["Searching State<br/>sdr.clear_buffer()"]
    H --> I["sdr.read Thread<br/>Continuous Operation"]
    
    I --> I1["Check free_size()"]
    I1 --> I2["write_head()"]
    I2 --> I3["Read from SDR or File"]
    I3 --> I4["commit()"]
    I4 --> I1
    
    G --> J["Cell Found?"]
    J -->|No| H
    J -->|Yes| K["Syncing State"]
    K --> L["phy.synchronize_subframe()"]
    L --> M["Processing State"]
    
    M --> N{Frame Type?}
    N -->|CAS Frame| O["phy.get_next_frame"]
    N -->|MBSFN Frame| O
    
    O --> P["sdr.get_samples()"]
    P --> P1["Check used_size()"]
    P1 --> P2["read buffer"]
    P2 --> P3["get capacity()"]
    P3 --> P4["Process frame"]
    
    P4 --> Q["get_buffer_level()"]
    Q --> Q1["Check used_size()"]
    Q1 --> Q2["Check capacity()"]
    
    N -->|Buffer Error| R["sdr.stop()"]
    R --> R1["clear_buffer()"]
    R1 --> H
    
    M --> S{"Restart?"}
    S -->|Yes| T["sdr.stop()"]
    T --> U["clear_buffer()"]
    U --> V["sdr.tune()"]
    V --> W["sdr.start()"]
    W --> I
    
    style A fill:#e1f5ff
    style D fill:#c8e6c9
    style I fill:#fff9c4
    style P fill:#f8bbd0
    style Q fill:#f3e5f5
    style R fill:#ffccbc