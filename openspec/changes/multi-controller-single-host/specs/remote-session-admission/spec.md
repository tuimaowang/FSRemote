## ADDED Requirements

### Requirement: Versioned admission precedes WebRTC negotiation
The stream host MUST complete a bounded, versioned admission handshake before accepting SDP, ICE candidates, control messages, or media-subscriber connections from a client.

#### Scenario: Compatible client completes negotiation
- **WHEN** a client offers a supported protocol version and compatible required capabilities
- **THEN** the host selects the version, records negotiated capabilities, and proceeds to authorization before WebRTC negotiation

#### Scenario: Client uses an unsupported version
- **WHEN** a client cannot negotiate a supported admission protocol version
- **THEN** the host returns an unsupported-version rejection and closes that session without creating a PeerConnection

#### Scenario: Client does not complete the handshake
- **WHEN** a connected client does not complete admission within the configured deadline
- **THEN** the host returns or records a timeout reason and closes only that client connection

### Requirement: Authorized device identity is proven
The stream host MUST admit a remote session only after the client proves possession of a private key whose exact public key is already present in the controlled device's authorized trust store.

#### Scenario: Authorized client proves its identity
- **WHEN** a client signs the host challenge and session context with the private key corresponding to an authorized public key
- **THEN** the host admits the identity subject to policy and capacity checks

#### Scenario: Client supplies an unknown public key
- **WHEN** a client presents a public key that is not in the controlled device's trust store
- **THEN** the host rejects the session as unauthorized before creating WebRTC or audio resources

#### Scenario: Signature does not match the challenge
- **WHEN** a client signature is invalid, replayed, or bound to different nonces, host identity, version, or requested role
- **THEN** the host rejects the session as unauthorized

### Requirement: Admission results are explicit and scoped
The host SHALL return an admitted session ID and scoped media credentials on success, or a structured rejection reason on failure, without exposing another session's credentials or state.

#### Scenario: Admission succeeds
- **WHEN** identity, protocol, policy, and capacity checks all succeed
- **THEN** the client receives a unique session ID, negotiated role state, capabilities, and short-lived scoped media credentials

#### Scenario: Admission is rejected by policy or capacity
- **WHEN** authorization succeeds but host policy or capacity prevents admission
- **THEN** the client receives the corresponding policy or capacity reason and no usable session credentials

### Requirement: Separate audio transport is bound to an admitted session
The audio listener MUST accept a subscriber only when it presents a valid, unexpired, single-use audio token associated with an active admitted session.

#### Scenario: Admitted session connects to audio
- **WHEN** an active session presents its valid audio token
- **THEN** the audio hub attaches one subscriber to that session and invalidates the one-time token

#### Scenario: Reused or foreign audio token is presented
- **WHEN** a client presents an expired, reused, invalid, or differently scoped audio token
- **THEN** the audio listener closes that connection without sending captured audio

