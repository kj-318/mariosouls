# DSR player bone table

Parsed 2026-10-20 from the game's own loose files with `tools/flver_tools.py`
(DSR models are FLVER2, version 0x2000C).

## How it maps to the GPU upload

The 2288-byte constant buffer holds the view-projection at offset 0 and the
bone palette from float 112 (byte 0x1C0), 12 floats (3x4) per slot. Slot j of
a draw is `mesh.bones[j]` of that mesh, an index into the PART file's own bone
list (not the skeleton's). Palette entries are skinning matrices, so a bone
near its rest pose gives the character's root transform.

**Slot 0 of every body and leg mesh is the Pelvis**; arms start at R_Forearm,
the head at Spine1. Verified on the `*_0000` parts only.

## Skeleton (chr/c0000.flver, 80 bones)

| # | Name | Parent |
|---|---|---|
| 0 | Master | - |
| 1 | Lower_Root | Master |
| 2 | Pelvis | Lower_Root |
| 3 | L_Thigh | Pelvis |
| 4 | L_Calf | L_Thigh |
| 5 | L_Foot | L_Calf |
| 6 | L_Toe0 | L_Foot |
| 7 | LCalfTwist | L_Thigh |
| 8 | R_Thigh | Pelvis |
| 9 | R_Calf | R_Thigh |
| 10 | R_Foot | R_Calf |
| 11 | R_Toe0 | R_Foot |
| 12 | RCalfTwist | R_Thigh |
| 13 | L_Wepon_Case | Pelvis |
| 14 | R_Wepon_Case | Pelvis |
| 15 | Upper_Root | Master |
| 16 | Spine | Upper_Root |
| 17 | Spine1 | Spine |
| 18 | Neck | Spine1 |
| 19 | Head | Neck |
| 20 | B_Wepon_Case | Spine1 |
| 21 | B_Wepon_Case0_L | Spine1 |
| 22 | B_Wepon_Case0_R | Spine1 |
| 23 | L_Clavicle | Spine1 |
| 24 | L_UpperArm | L_Clavicle |
| 25 | L_Forearm | L_UpperArm |
| 26 | L_Hand | L_Forearm |
| 27 | L_Finger0 | L_Hand |
| 28 | L_Finger01 | L_Finger0 |
| 29 | L_Finger02 | L_Finger01 |
| 30 | L_Finger1 | L_Hand |
| 31 | L_Finger11 | L_Finger1 |
| 32 | L_Finger12 | L_Finger11 |
| 33 | L_Finger2 | L_Hand |
| 34 | L_Finger21 | L_Finger2 |
| 35 | L_Finger22 | L_Finger21 |
| 36 | L_Weapon | L_Hand |
| 37 | L_Shoulderpad | L_UpperArm |
| 38 | L_ForeTwist | L_UpperArm |
| 39 | LUpArmTwist | L_Clavicle |
| 40 | R_Clavicle | Spine1 |
| 41 | R_UpperArm | R_Clavicle |
| 42 | R_Forearm | R_UpperArm |
| 43 | R_Hand | R_Forearm |
| 44 | R_Finger0 | R_Hand |
| 45 | R_Finger01 | R_Finger0 |
| 46 | R_Finger02 | R_Finger01 |
| 47 | R_Finger1 | R_Hand |
| 48 | R_Finger11 | R_Finger1 |
| 49 | R_Finger12 | R_Finger11 |
| 50 | R_Finger2 | R_Hand |
| 51 | R_Finger21 | R_Finger2 |
| 52 | R_Finger22 | R_Finger21 |
| 53 | R_Weapon | R_Hand |
| 54 | R_Shoulderpad | R_UpperArm |
| 55 | R_ForeTwist | R_UpperArm |
| 56 | RUpArmTwist | R_Clavicle |
| 57 | L_Po | Spine1 |
| 58 | R_Po | Spine1 |
| 59 | Model_Dmy | - |
| 60 | c2030_sfx_dummy | - |
| 61 | 破損用sfxダミー | - |
| 62 | パリィ用 | - |
| 63 | 体力ゲージ | - |
| 64 | 宝死体SFX | - |
| 65 | 投げ＆新ファントムテスト | - |
| 66 | 間接ジオメトリ用ダミー | - |
| 67 | ライト魔法 | - |
| 68 | オブジェクト01 | - |
| 69 | L_Toe0Nub | - |
| 70 | R_Toe0Nub | - |
| 71 | HeadNub | - |
| 72 | L_Finger0Nub | - |
| 73 | L_Finger1Nub | - |
| 74 | L_Finger2Nub | - |
| 75 | L_Shield | - |
| 76 | R_Finger0Nub | - |
| 77 | R_Finger1Nub | - |
| 78 | R_Finger2Nub | - |
| 79 | R_Shield | - |

## Parts: per-mesh palette slots

### BD_M_0000
- mesh 0: 0=Pelvis, 1=Spine, 2=Spine1, 3=R_Clavicle, 4=R_UpperArm, 5=R_Forearm, 6=R_ForeTwist, 7=RUpArmTwist, 8=L_Clavicle, 9=L_UpperArm, 10=L_Forearm, 11=L_ForeTwist, 12=LUpArmTwist, 13=Neck

### LG_M_0000
- mesh 0: 0=Pelvis, 1=R_Thigh, 2=R_Calf, 3=R_Foot, 4=R_Toe0, 5=RCalfTwist, 6=L_Thigh, 7=L_Calf, 8=L_Foot, 9=L_Toe0, 10=LCalfTwist, 11=Spine
- mesh 1: 0=Pelvis, 1=R_Thigh, 2=L_Thigh, 3=Spine
- mesh 2: 0=Pelvis, 1=R_Thigh, 2=L_Thigh, 3=Spine
- mesh 3: 0=Pelvis, 1=R_Thigh, 2=L_Thigh, 3=Spine

### AM_M_0000
- mesh 0: 0=R_Forearm, 1=R_Hand, 2=R_Finger0, 3=R_Finger01, 4=R_Finger02, 5=R_Finger1, 6=R_Finger11, 7=R_Finger12, 8=R_Finger2, 9=R_Finger21, 10=R_Finger22, 11=R_ForeTwist, 12=L_Forearm, 13=L_Hand, 14=L_Finger0, 15=L_Finger01, 16=L_Finger02, 17=L_Finger1, 18=L_Finger11, 19=L_Finger12, 20=L_Finger2, 21=L_Finger21, 22=L_Finger22, 23=L_ForeTwist

### HD_M_0000
- mesh 0: 0=Spine1, 1=Neck, 2=L_Clavicle


## Rule check across a sample (2026-10-20)

32 random armour parts (body, legs, arms, head; male, female, shared), 95
meshes using the Pelvis: **Pelvis was slot 0 in all 95, no exceptions.**
Meshes without the pelvis start with another bone (Spine1 in 14 body and 12
head meshes; forearms, hands, thighs or calves otherwise). Spine1 near its rest
pose also lands near the root, so when picking the repeated slot-0 matrix the
pelvis must out-count it; leg meshes all carry the pelvis, so it normally does.
