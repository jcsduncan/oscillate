oscillate_add_library(engine
  SOURCES audio_worker_pool.cpp graph.cpp chunked_processor.cpp midi_events.cpp
          routing_latency.cpp delay_compensation.cpp routed_mixer.cpp
          routed_node_meters.cpp routed_publisher.cpp
  PUBLIC oscillate::foundation oscillate::project)
