# Support Intel Arc (A730M / Alchemist / Battlemage)

Ce document décrit les adaptations effectuées dans **Veyra-NRVideo** pour assurer une compatibilité complète et performante avec les cartes graphiques **Intel Arc**, et en particulier l'**Intel Arc A730M** (Alchemist ACM-G10, 24 Xe-cores, moteurs matriciels XMX, 12 Go GDDR6).

---

## 1. Fonctionnalités prises en charge sur Intel Arc A730M

| Domaine | État sur Intel Arc A730M | Détails |
|---|---|---|
| **Sélection GPU hybride (iGPU + Arc dGPU)** | Natif | Détection automatique de l'Arc A730M (`0x8086` / `0x5691`) et priorité au GPU discret sur les PC portables dotés d'un iGPU Intel Iris Xe / UHD. Lecture automatique de la version du pilote Intel (`igdkmdnd64` / `igdkmdn64`). |
| **Décodage matériel vidéo** | Natif (`D3D12VA`) | H.264, HEVC 8/10-bit, AV1, VP9 sur le périphérique D3D12 partagé de Veyra. |
| **Super Résolution : Intel XeSS SR (XMX)** | Natif (XMX) | Nouveau backend `XessSrBackend` (`libxess.dll`), exploitant les moteurs matriciels **XMX** de l'Arc A730M (SDR, HDR linéaire, simple et double SR). |
| **Super Résolution : AMD FSR 3.1 & NVIDIA NIS** | Natif (D3D12) | Disponibles en alternative ou en double SR. |
| **Génération d'images : Intel XeSS FG (2X–4X)** | Natif Intel | `XessPresenter` détecte désormais les GPU Intel et ne rejette plus les DLL officielles Intel XeSS 3.x lors d'une demande 3X/4X (interrogation directe de `xefgSwapChainGetProperties`, avec support du patch 1.3.1.78 si présent). |
| **Génération d'images : FSR 3.1 (2X), RIFE DirectML (2X/3X), ANR FG (2X–6X XMX)** | Natif | Tous supportés sur D3D12 ; ANR FG embarque déjà des programmes XMX (`fg-lite-any-portable-xmx-*`). |
| **Flux optique (Optical Flow)** | Corrigé | Bascule automatique vers **AMD FidelityFX Optical Flow (D3D12)** ou **GPU-DIS** sur Intel (`0x8086`), évitant tout appel à NVIDIA NVOF. |
| **Export & Enregistrement matériel (Intel Quick Sync)** | Natif 8-bit & 10-bit HDR | `MfVideoEncoder` supporte désormais l'encodage matériel **H.264 / HEVC 8-bit (NV12)** et **HEVC Main10 10-bit HDR (P010, BT.2020 PQ)** via les MFT Intel Quick Sync. |
| **Post-traitement & Sources** | Natif | Débruitage vidéo (`VideoDenoise.dxil`), Anti-aliasing (FXAA/SMAA/MLAA), Colorimétrie / LUTs / Dolby Vision, Beauté, Capture HDMI, PS5 Remote Play, Xbox, Moonlight/Sunshine, Virtual Camera. |
| **Neural Rendering (DLSS 5 NR / AMD HIP NR)** | Non applicable | Les runtimes NR existants sont propriétaires NVIDIA NGX (CUDA/Tensor Cores) ou AMD ROCm/HIP (RX 7000/9000) et restent grisés sur Intel. |

---

## 2. Compiler le binaire Windows (.exe)

### Option A — GitHub Actions (recommandé, zéro configuration locale)

1. Poussez ce dépôt sur votre propre dépôt / fork GitHub.
2. Ouvrez l'onglet **Actions** → **Windows build (reproducible)** → **Run workflow**.
3. Laissez cochées :
   - `include_intel_xess_runtime = true` (embarque automatiquement `libxess.dll`, `libxess_fg.dll`, `libxell.dll` depuis Intel XeSS SDK 3.0.2, les DLL FidelityFX et ONNX Runtime / DirectML)
   - `include_rife_dml_headers = true` (compile le support RIFE DirectML)
4. Une fois le workflow terminé, téléchargez l'artefact **`veyra-windows-ci-no-gpu-runtimes`** (archive ZIP portable Windows x64), extrayez-le et lancez **`veyra_qml_ui.exe`**.

### Option B — Compilation locale sur PC Windows 11 (MSVC 2022)

Prérequis : Visual Studio 2022 (C++ Desktop), CMake, Ninja, Qt 6.8.3 (`msvc2022_64`), Git, Rust (`cargo-c` pour `libdovi`), vcpkg.

```powershell
# 1. Récupérer les SDK (dont Intel XeSS SDK 3.0.2 dans third_party_local/intel/xess-3.0.2)
.\scripts\ci\fetch-sdks.ps1 -VeyraRoot . -WorkRoot C:\sdk-src -ManifestOut C:\sdk-manifest.json

# 2. (Optionnel) Télécharger les en-têtes ONNX Runtime DirectML + DirectML pour RIFE
New-Item -ItemType Directory -Force C:\ort-dml\ort, C:\ort-dml\dml | Out-Null
Invoke-WebRequest https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4 -OutFile C:\ort-dml\ort.nupkg
Invoke-WebRequest https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4 -OutFile C:\ort-dml\dml.nupkg
Expand-Archive C:\ort-dml\ort.nupkg C:\ort-dml\ort -Force
Expand-Archive C:\ort-dml\dml.nupkg C:\ort-dml\dml -Force

# 3. Configurer et compiler
cmake --preset x64-release -B build-intel `
  -DVEYRA_BUILD_QML_UI=ON `
  -DVEYRA_ORT_DML_ROOT=C:/ort-dml `
  -DVEYRA_FFMPEG_ROOT=C:/v/installed/x64-windows `
  -DVEYRA_LIBASS_ROOT=C:/v/installed/x64-windows-static `
  -DVEYRA_LIBDOVI_ROOT=C:/dovi `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64

cmake --build build-intel --parallel --target veyra_qml_ui veyra_effect_availability_tests veyra_repair_contract_tests veyra_video_ai_settings_tests
```

---

## 3. Arborescence des DLL runtime pour Intel Arc A730M

À côté de `veyra_qml_ui.exe` :

```text
veyra_qml_ui.exe
runtime_local/
├── intel/experimental/      (ou intel/xess/, ou runtime/intel/)
│   ├── libxess.dll          # Intel XeSS Super Resolution (XMX sur Arc A730M)
│   ├── libxess_fg.dll       # Intel XeSS Frame Generation
│   └── libxell.dll          # Intel Xe Low Latency (requis par XeSS FG)
└── amd/fidelityfx/
    ├── amd_fidelityfx_loader_dx12.dll
    ├── amd_fidelityfx_upscaler_dx12.dll
    └── amd_fidelityfx_framegeneration_dx12.dll
runtime/
└── rife/                    # Optionnel : voir docs/RIFE_DIRECTML.md
    ├── DirectML.dll
    ├── onnxruntime.dll
    ├── onnxruntime_providers_shared.dll
    ├── rife_v4.26_fp16.onnx
    └── rife_v4.22_lite_fp16.onnx
```

---

## 4. Réglages recommandés pour Intel Arc A730M

- **Pilote Windows** : utilisez un pilote Intel Arc récent (Windows 11 x64, Resizable BAR activé si supporté par le BIOS du portable).
- **Super Résolution** : **Intel XeSS SR (XMX)** (par défaut lorsque `libxess.dll` est présent) ou **FSR 3.1**.
- **Génération d'images** :
  - Lecture / capture temps réel : **Intel XeSS FG (2X)** (ou 3X/4X selon la version du runtime) ou **FSR 3.1 (2X)**.
  - Meilleure qualité d'interpolation vidéo / export hors-ligne : **RIFE DirectML (2X / 3X)** (SDR) ou **FSR 3.1 (2X)**.
- **Flux optique** : **AMD FidelityFX** (D3D12), avec l'option demi-résolution si nécessaire en 4K.
- **Export HDR** : sélectionnez **HEVC** — Veyra utilise automatiquement le chemin **P010 HEVC Main10** via l'encodeur matériel Intel Quick Sync.
- **Variable de diagnostic** : `VEYRA_XESS_NATIVE_INTEL=1` force le chemin natif Intel XeSS-FG sans appliquer le patch mémoire `1.3.1.78`.
