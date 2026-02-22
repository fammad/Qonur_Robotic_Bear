# Qonur 3D Print Pack

This repository contains the **mechanical design package** for Qonur:
- a **ready-to-print 3MF pack**
- a **Fusion 360 editable design file**
- supporting documentation PDFs
- preview images

---

## Preview

### Exterior views (front / back)
![Qonur front side back](images/Image_Qonur_front-back.png)

### Internal mechanism highlight (eye mechanism view)
![Qonur internal eye mechanism](images/Image_Qonur_Internal_Eye_Mechanism.png)

### Print pack in slicer (pre-arranged plates)
![Qonur print pack in slicer](images/Image_Qonur_Print_Pack_in_slicer.png)

---

## Download

- Easiest: **Code → Download ZIP**
- Then open the folders below depending on what you want to do.

---

## What you can do

### If you want to print (fastest)
Go to: `printable/`

- `Printable_Robotic_Components.3mf`  
  Open this in your slicer and print.

### If you want to edit the design
Go to: `editable/`

- `Editable_Components_Fusion360.f3z`  
  Import into Fusion 360, modify, and export new print files.

### If you want documentation
Go to: `docs/`

- `Qonur_InfoPack_ENG.pdf`  
- `Qonur_Physcial_Design.pdf`

---

## Tested printing setup

- CAD: Autodesk Fusion 360
- Printer: Elegoo Neptune 4 Pro
- Nozzle: 0.4 mm
- Material: PLA recommended

Baseline slicer settings (safe starting point):
- Layer height: 0.2 mm (0.16 mm for smoother finish)
- Walls/perimeters: 3
- Infill: 10–20%
- Supports: only where needed
- Brim: optional for stability on curved parts

---

## Notes about the physical design

The design includes dedicated placeholders/openings that support assembly and future component placement:
- Ear area: microphone placeholder
- Back access: cable/charging access
- Chest: LED indicator placeholder
- Chest circular mount: speaker placement area

Assembly can be done with glue or with printed connectors depending on your preferred build approach.

---

## Help

If a part does not fit or you get print failures, open a GitHub Issue and include:
- printer + material
- slicer name + key settings
- which file you printed
- photos of the issue
