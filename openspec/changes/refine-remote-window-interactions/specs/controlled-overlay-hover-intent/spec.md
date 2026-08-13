## ADDED Requirements

### Requirement: Controlled-device bubble expansion requires hover intent
The controlled-device status overlay SHALL expand from its collapsed badge only after the cursor remains within the stable collapsed-badge geometry for a short hover-intent delay.

#### Scenario: Cursor rapidly crosses the collapsed badge
- **WHEN** the cursor enters and leaves the collapsed badge before the hover-intent delay expires
- **THEN** the overlay remains collapsed and no expansion animation starts

#### Scenario: Cursor deliberately hovers over the collapsed badge
- **WHEN** the cursor remains inside the collapsed badge for the complete hover-intent delay
- **THEN** the overlay expands and continues using the full expanded card for leave detection

#### Scenario: A controller session first appears
- **WHEN** the controlled-device controller list changes from empty to non-empty
- **THEN** the complete status bubble appears immediately without waiting for hover intent
