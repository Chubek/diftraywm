<!-- This file is auto-generated. Do not edit manually. -->
<!-- Run: python3 scripts/systems_reference.py --force -->

# angle Hierarchy

**Systems:** Angular, HEP, ISQ Angle

**Dimension:** dim_angle

```mermaid
flowchart LR
    angular_angle["<b><a href="../../systems/angular/#angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">angular::angle</a></b>"]
    hep_azimuthal_angle["<b><a href="../../systems/hep/#azimuthal_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">hep::azimuthal_angle</a></b>"]
    angular_angle --- hep_azimuthal_angle
    hep_opening_angle["<b><a href="../../systems/hep/#opening_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">hep::opening_angle</a></b>"]
    angular_angle --- hep_opening_angle
    hep_polar_angle["<b><a href="../../systems/hep/#polar_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">hep::polar_angle</a></b>"]
    angular_angle --- hep_polar_angle
    hep_scattering_angle["<b><a href="../../systems/hep/#scattering_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">hep::scattering_angle</a></b>"]
    angular_angle --- hep_scattering_angle
    isq_angle_angular_measure["<b><a href="../../systems/isq_angle/#angular_measure" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::angular_measure</a></b><br><i>(<a href="../../systems/isq_angle/#cotes_angle_constant" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">cotes_angle_constant</a> * isq::arc_length / isq::radius)</i>"]
    angular_angle --- isq_angle_angular_measure
    isq_angle_loss_angle["<b><a href="../../systems/isq_angle/#loss_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::loss_angle</a></b>"]
    isq_angle_angular_measure --- isq_angle_loss_angle
    isq_angle_phase_angle["<b><a href="../../systems/isq_angle/#phase_angle" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::phase_angle</a></b>"]
    isq_angle_angular_measure --- isq_angle_phase_angle
    isq_angle_cotes_angle_constant["<b><a href="../../systems/isq_angle/#cotes_angle_constant" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::cotes_angle_constant</a></b>"]
    angular_angle --- isq_angle_cotes_angle_constant
    isq_angle_rotational_displacement["<b><a href="../../systems/isq_angle/#rotational_displacement" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::rotational_displacement</a></b><br><i>(<a href="../../systems/isq_angle/#cotes_angle_constant" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">cotes_angle_constant</a> * isq::path_length / isq::radius)</i>"]
    angular_angle --- isq_angle_rotational_displacement
    isq_angle_rotation["<b><a href="../../systems/isq_angle/#rotation" style="color: var(--md-mermaid-label-fg-color); text-decoration: none;">isq_angle::rotation</a></b>"]
    isq_angle_rotational_displacement --- isq_angle_rotation
```
