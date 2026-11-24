# Get the Megascans models for the cluster mesh demo

`tests/clustermesh.earth` scatters four rocks and two plants from Quixel Megascans over the
terrain. The models are not stored in this repository. They are free on [Fab](https://www.fab.com)
under the Fab Standard License, and you need an Epic Games account to download them.

## Download the files

1. Sign in at fab.com.
2. Open each listing in the table and click **Download**.
3. In the list of files, download the one named in the table. Save all six files in one folder.

| Model | Listing | File to download | Size |
| --- | --- | --- | --- |
| Mossy Forest Boulder | [listing](https://www.fab.com/listings/9fe77d07-75a8-43eb-afed-058b157fdcf9) | `mossy_forest_boulder_wfstchwaw_raw.glb` | 246 MB |
| Icelandic Rock Formation | [listing](https://www.fab.com/listings/9eee408a-3efe-4494-9f5c-988b0a16c9e9) | `icelandic_rock_formation_tfiqaftda_raw.glb` | 217 MB |
| Canyon Sandstone Boulder | [listing](https://www.fab.com/listings/366026f1-13b7-42e8-b0e1-6d64f6363440) | `canyon_sandstone_boulder_ulmtabz_raw.glb` | 172 MB |
| Massive Nordic Coastal Cliff | [listing](https://www.fab.com/listings/fea6c4dd-7005-4ca1-89b3-6fff4e35c5e8) | `massive_nordic_coastal_cliff_veqnci0db_raw.glb` | 344 MB |
| Lady Fern | [listing](https://www.fab.com/listings/3eead001-af7f-4b65-afcc-89c368f089bc) | `lady_fern_wdvlditia_ue_high.zip` | 84 MB |
| Tundra Grass | [listing](https://www.fab.com/listings/3d87458a-9492-4098-94e9-e45cc593f00f) | `tundra_grass_wk2oaeyqx_ue_raw.zip` | 133 MB |

The rock files are in the **glb** group of the list. The plant files are the `.zip` files
whose names contain `_ue_`. Do not unzip them.

## Prepare the models

The preparation script needs Python 3 and Pillow.

```
pip install pillow
python data/megascans/prepare.py <folder with the six files>
```

The script takes about 20 seconds. It writes one folder per model next to itself, about
400 MB in total. Git ignores those folders.

The script does two things:

- It resizes the textures from 8K to 4096 pixels for the rocks and to 2048 pixels for the
  plants. The models then load several times faster. The geometry is not changed.
- It removes the lower-detail copies of each plant variant from the plant files. A cluster
  mesh makes its own levels of detail.

If a file is missing, the script names it and prepares the others.

## Run the demo

```
osgearth_imgui tests/clustermesh.earth --nvgl
```

The first run converts each model to a cluster mesh and stores the result next to the model
as an `.oecm` file. Later runs read those files and load the models in about 10 seconds.
