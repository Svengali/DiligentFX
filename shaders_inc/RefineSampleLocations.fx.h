"// RefineSampleLocations.fx\n"
"// Refines sample locations using inscattering difference or z difference\n"
"\n"
"#include \"BasicStructures.fxh\"\n"
"#include \"AtmosphereShadersCommon.fxh\"\n"
"\n"
"// In fact we only need RG16U texture to store\n"
"// interpolation source indices. However, NVidia GLES does\n"
"// not supported imge load/store operations on this format, \n"
"// so we have to resort to RGBA32U.\n"
"RWTexture2D<uint2/*format = rgba32ui*/> g_rwtex2DInterpolationSource;\n"
"\n"
"Texture2D<float2> g_tex2DCoordinates;\n"
"Texture2D<float>  g_tex2DEpipolarCamSpaceZ;\n"
"Texture2D<float3> g_tex2DScatteredColor;\n"
"Texture2D<float>  g_tex2DAverageLuminance;\n"
"\n"
"cbuffer cbPostProcessingAttribs\n"
"{\n"
"    EpipolarLightScatteringAttribs g_PPAttribs;\n"
"};\n"
"\n"
"#include \"ToneMapping.fxh\"\n"
"\n"
"#ifndef INITIAL_SAMPLE_STEP\n"
"#   define INITIAL_SAMPLE_STEP 128\n"
"#endif\n"
"\n"
"#ifndef THREAD_GROUP_SIZE\n"
"#   define THREAD_GROUP_SIZE max(INITIAL_SAMPLE_STEP, 32)\n"
"#endif\n"
"\n"
"#ifndef REFINEMENT_CRITERION\n"
"#   define REFINEMENT_CRITERION REFINEMENT_CRITERION_INSCTR_DIFF\n"
"#endif\n"
"\n"
"// In my first implementation I used group shared memory to store camera space z\n"
"// values. This was a very low-performing method\n"
"// After that I tried using arrays of bool flags instead, but this did not help very much\n"
"// since memory bandwidth was almost the same (on GPU each bool consumes 4 bytes)\n"
"// Finally, I came up with packing 32 flags into single uint value.\n"
"// This not only enables using 32x times less memory, but also enables very efficient\n"
"// test if depth break is present in the section\n"
"#define NUM_PACKED_FLAGS (THREAD_GROUP_SIZE/32)\n"
"groupshared uint g_uiPackedCamSpaceDiffFlags[ NUM_PACKED_FLAGS ];\n"
"#if REFINEMENT_CRITERION == REFINEMENT_CRITERION_INSCTR_DIFF\n"
"groupshared float3 g_f3Inscattering[THREAD_GROUP_SIZE+1];\n"
"#endif\n"
"\n"
"[numthreads(THREAD_GROUP_SIZE, 1, 1)]\n"
"void RefineSampleLocationsCS(uint3 Gid  : SV_GroupID, \n"
"                             uint3 GTid : SV_GroupThreadID)\n"
"{\n"
"    // Each thread group processes one slice\n"
"    uint uiSliceInd = Gid.y;\n"
"    // Compute global index of the first sample in the thread group\n"
"    // Each group processes THREAD_GROUP_SIZE samples in the slice\n"
"    uint uiGroupStartGlobalInd = Gid.x * uint(THREAD_GROUP_SIZE);\n"
"    uint uiSampleInd = GTid.x; // Sample index in the group\n"
"    // Compute global index of this sample which is required to fetch the sample\'s coordinates\n"
"    uint uiGlobalSampleInd = uiGroupStartGlobalInd + uiSampleInd;\n"
"    // Load location of the current sample using global sample index\n"
"    float2 f2SampleLocationPS = g_tex2DCoordinates.Load( int3(uiGlobalSampleInd, uiSliceInd, 0) );\n"
"    \n"
"    bool bIsValidThread = all( Less( abs(f2SampleLocationPS), (1.0 + 1e-4)*float2(1.0,1.0) ) );\n"
"\n"
"    // Initialize flags with zeroes\n"
"    if( GTid.x < uint(NUM_PACKED_FLAGS) )\n"
"        g_uiPackedCamSpaceDiffFlags[GTid.x] = 0u;\n"
"    \n"
"    GroupMemoryBarrierWithGroupSync();\n"
"\n"
"    // Let each thread in the group compute its own flag\n"
"    // Note that if the sample is located behind the screen, its flag will be set to zero\n"
"    // Besides, since g_tex2DEpipolarCamSpaceZ is cleared with invalid coordinates, the difference\n"
"    // flag between valid and invalid locations will also be zero. Thus the sample next to invalid will always\n"
"    // be marked as ray marching sample\n"
"    [branch]\n"
"    if( bIsValidThread )\n"
"    {\n"
"#if REFINEMENT_CRITERION == REFINEMENT_CRITERION_DEPTH_DIFF\n"
"        // Load camera space Z for this sample and for its right neighbour (remeber to use global sample index)\n"
"        float fCamSpaceZ =            g_tex2DEpipolarCamSpaceZ.Load( int3(uiGlobalSampleInd,         uiSliceInd, 0) );\n"
"        float fRightNeighbCamSpaceZ = g_tex2DEpipolarCamSpaceZ.Load( int3( int(uiGlobalSampleInd)+1, uiSliceInd, 0) );\n"
"        float fMaxZ = max(fCamSpaceZ, fRightNeighbCamSpaceZ);\n"
"        fMaxZ = max(fMaxZ, 1.0);\n"
"        // Compare the difference with the threshold\n"
"        bool bFlag = abs(fCamSpaceZ - fRightNeighbCamSpaceZ)/fMaxZ < 0.2*g_PPAttribs.fRefinementThreshold;\n"
"#elif REFINEMENT_CRITERION == REFINEMENT_CRITERION_INSCTR_DIFF\n"
"        // Load inscattering for this sample and for its right neighbour\n"
"        float3 f3Insctr0 = g_tex2DScatteredColor.Load( int3(uiGlobalSampleInd,         uiSliceInd, 0) );\n"
"        float3 f3Insctr1 = g_tex2DScatteredColor.Load( int3( int(uiGlobalSampleInd)+1, uiSliceInd, 0) );\n"
"        float3 f3MaxInsctr = max(f3Insctr0, f3Insctr1);\n"
"        \n"
"        // Compute minimum inscattering threshold based on the average scene luminance\n"
"        float fAverageLum = GetAverageSceneLuminance(g_tex2DAverageLuminance);\n"
"        // Inscattering threshold should be proportional to the average scene luminance and\n"
"        // inversely proportional to the middle gray level (the higher middle gray, the briter the scene,\n"
"        // thus the less the theshold)\n"
"        // It should also account for the fact that rgb channels contribute differently\n"
"        // to the percieved brightness. For r channel the threshold should be smallest, \n"
"        // for b channel - the largest\n"
"        float3 f3MinInsctrThreshold = (0.02 * fAverageLum * F3ONE / RGB_TO_LUMINANCE.xyz) / g_PPAttribs.ToneMapping.fMiddleGray;\n"
"\n"
"        f3MaxInsctr = max(f3MaxInsctr, f3MinInsctrThreshold);\n"
"        // Compare the difference with the threshold. If the neighbour sample is invalid, its inscattering\n"
"        // is large negative value and the difference is guaranteed to be larger than the threshold\n"
"        bool bFlag = all( Less(abs(f3Insctr0 - f3Insctr1)/f3MaxInsctr, g_PPAttribs.fRefinementThreshold*float3(1.0,1.0,1.0) ) );\n"
"#endif\n"
"        // Set appropriate flag using INTERLOCKED Or:\n"
"        uint uiBit = bFlag ? (1u << (uiSampleInd % 32u)) : 0u;\n"
"        InterlockedOr( g_uiPackedCamSpaceDiffFlags[int(uiSampleInd)/32], uiBit );\n"
"    }\n"
"\n"
"    // Synchronize threads in the group\n"
"    GroupMemoryBarrierWithGroupSync();\n"
"    \n"
"    // Skip invalid threads. This can be done only after the synchronization\n"
"    if( !bIsValidThread )\n"
"        return;\n"
"\n"
"    //                                 uiInitialSampleStep\n"
"    //       uiSampleInd             |<--------->|\n"
"    //          |                    |           |\n"
"    //       X  *  *  *  X  *  *  *  X  *  *  *  X           X - locations of initial samples\n"
"    //       |           |\n"
"    //       |           uiInitialSample1Ind\n"
"    //      uiInitialSample0Ind\n"
"    //\n"
"    // Find two closest initial ray marching samples\n"
"    uint uiInitialSampleStep = uint(INITIAL_SAMPLE_STEP);\n"
"    uint uiInitialSample0Ind = (uiSampleInd / uiInitialSampleStep) * uiInitialSampleStep;\n"
"    // Use denser sampling near the epipole to account for high variation\n"
"    // Note that sampling near the epipole is very cheap since only a few steps\n"
"    // are required to perform ray marching\n"
"    uint uiInitialSample0GlobalInd = uiInitialSample0Ind + uiGroupStartGlobalInd;\n"
"    float2 f2InitialSample0Coords = g_tex2DCoordinates.Load( int3(uiInitialSample0GlobalInd, uiSliceInd, 0) );\n"
"    if( float(uiInitialSample0GlobalInd)/float(MAX_SAMPLES_IN_SLICE) < 0.05 && \n"
"        length(f2InitialSample0Coords - g_PPAttribs.f4LightScreenPos.xy) < 0.1 )\n"
"    {\n"
"        uiInitialSampleStep = max( uint(INITIAL_SAMPLE_STEP) / g_PPAttribs.uiEpipoleSamplingDensityFactor, 1u );\n"
"        uiInitialSample0Ind = (uiSampleInd / uiInitialSampleStep) * uiInitialSampleStep;\n"
"    }\n"
"    uint uiInitialSample1Ind = uiInitialSample0Ind + uiInitialSampleStep;\n"
"\n"
"    // Remeber that the last sample in each epipolar slice must be ray marching one\n"
"    uint uiInterpolationTexWidth, uiInterpolationTexHeight;\n"
"    g_rwtex2DInterpolationSource.GetDimensions(uiInterpolationTexWidth, uiInterpolationTexHeight);\n"
"    if( Gid.x == uiInterpolationTexWidth/uint(THREAD_GROUP_SIZE) - 1u )\n"
"        uiInitialSample1Ind = min(uiInitialSample1Ind, uint(THREAD_GROUP_SIZE-1) );\n"
"\n"
"    uint uiLeftSrcSampleInd  = uiSampleInd;\n"
"    uint uiRightSrcSampleInd = uiSampleInd;\n"
"\n"
"    // Do nothing if sample is one of initial samples. In this case the sample will be \n"
"    // interpolated from itself\n"
"    if( uiSampleInd > uiInitialSample0Ind && uiSampleInd < uiInitialSample1Ind )\n"
"    {\n"
"        // Load group shared memory to the thread local memory\n"
"        uint uiPackedCamSpaceDiffFlags[ NUM_PACKED_FLAGS ];\n"
"        for(int i=0; i < NUM_PACKED_FLAGS; ++i)\n"
"            uiPackedCamSpaceDiffFlags[i] = g_uiPackedCamSpaceDiffFlags[i];\n"
"    \n"
"        // Check if there are no depth breaks in the whole section\n"
"        // In such case all the flags are set\n"
"        bool bNoDepthBreaks = true;\n"
"#if INITIAL_SAMPLE_STEP < 32\n"
"        {\n"
"            // Check if all uiInitialSampleStep flags starting from\n"
"            // position uiInitialSample0Ind are set:\n"
"            int iFlagPackOrder = int(uiInitialSample0Ind / 32u);\n"
"            int iFlagOrderInPack = int(uiInitialSample0Ind % 32u);\n"
"            uint uiFlagPack = uiPackedCamSpaceDiffFlags[iFlagPackOrder];\n"
"            uint uiAllFlagsMask = ((1u<<uiInitialSampleStep) - 1u);\n"
"            if( ((uiFlagPack >> uint(iFlagOrderInPack)) & uiAllFlagsMask) != uiAllFlagsMask )\n"
"                bNoDepthBreaks = false;\n"
"        }\n"
"#else\n"
"        {\n"
"            for(int i=0; i < NUM_PACKED_FLAGS; ++i)\n"
"                if( uiPackedCamSpaceDiffFlags[i] != 0xFFFFFFFFU )\n"
"                    // If at least one flag is not set, there is a depth break on this section\n"
"                    bNoDepthBreaks = false;\n"
"        }\n"
"#endif\n"
"\n"
"        if( bNoDepthBreaks )\n"
"        {\n"
"            // If there are no depth breaks, we can skip all calculations\n"
"            // and use initial sample locations as interpolation sources:\n"
"            uiLeftSrcSampleInd = uiInitialSample0Ind;\n"
"            uiRightSrcSampleInd = uiInitialSample1Ind;\n"
"        }\n"
"        else\n"
"        {\n"
"            // Find left interpolation source\n"
"            {\n"
"                // Note that i-th flag reflects the difference between i-th and (i+1)-th samples:\n"
"                // Flag[i] = abs(fCamSpaceZ[i] - fCamSpaceZ[i+1]) < g_PPAttribs.fRefinementThreshold;\n"
"                // We need to find first depth break starting from iFirstDepthBreakToTheLeftInd sample\n"
"                // and going to the left up to uiInitialSample0Ind\n"
"                int iFirstDepthBreakToTheLeftInd = int(uiSampleInd)-1;\n"
"                //                                                              iFirstDepthBreakToTheLeftInd\n"
"                //                                                                     |\n"
"                //                                                                     V\n"
"                //      0  1  2  3                       30 31   32 33     ....   i-1  i  i+1 ....  63   64\n"
"                //   |                                         |                           1  1  1  1  |\n"
"                //          uiPackedCamSpaceDiffFlags[0]             uiPackedCamSpaceDiffFlags[1]\n"
"                //\n"
"                //   iFlagOrderInPack == i % 32\n"
"\n"
"                int iFlagPackOrder = int( uint(iFirstDepthBreakToTheLeftInd) / 32u );\n"
"                int iFlagOrderInPack = int( uint(iFirstDepthBreakToTheLeftInd) % 32u );\n"
"                uint uiFlagPack = uiPackedCamSpaceDiffFlags[iFlagPackOrder];\n"
"                // To test if there is a depth break in the current flag pack,\n"
"                // we must check all flags starting from the iFlagOrderInPack\n"
"                // downward to 0 position. We must skip all flags from iFlagOrderInPack+1 to 31\n"
"                if( iFlagOrderInPack < 31 )\n"
"                {\n"
"                    // Set all higher flags to 1, so that they will be skipped\n"
"                    // Note that if iFlagOrderInPack == 31, there are no flags to skip\n"
"                    // Note also that (U << 32) != 0 as it can be expected. (U << 32) == U instead\n"
"                    uiFlagPack |= ( uint(0x0FFFFFFFFU) << uint(iFlagOrderInPack+1) );\n"
"                }\n"
"                // Find first zero flag starting from iFlagOrderInPack position. Since all\n"
"                // higher bits are set, they will be effectivelly skipped\n"
"                int iFirstUnsetFlagPos = firstbithigh( uint(~uiFlagPack) );\n"
"                // firstbithigh(0) == +INT_MAX\n"
"                if( !(0 <= iFirstUnsetFlagPos && iFirstUnsetFlagPos < 32) )\n"
"                    // There are no set flags => proceed to the next uint flag pack\n"
"                    iFirstUnsetFlagPos = -1;\n"
"                iFirstDepthBreakToTheLeftInd -= iFlagOrderInPack - iFirstUnsetFlagPos;\n"
"\n"
"#if INITIAL_SAMPLE_STEP > 32\n"
"                // Check the remaining full flag packs\n"
"                iFlagPackOrder--;\n"
"                while( iFlagPackOrder >= 0 && iFirstUnsetFlagPos == -1 )\n"
"                {\n"
"                    uiFlagPack = uiPackedCamSpaceDiffFlags[iFlagPackOrder];\n"
"                    iFirstUnsetFlagPos = firstbithigh( uint(~uiFlagPack) );\n"
"                    if( !(0 <= iFirstUnsetFlagPos && iFirstUnsetFlagPos < 32) )\n"
"                        iFirstUnsetFlagPos = -1;\n"
"                    iFirstDepthBreakToTheLeftInd -= 31 - iFirstUnsetFlagPos;\n"
"                    iFlagPackOrder--;\n"
"                }\n"
"#endif\n"
"                // Ray marching sample is located next to the identified depth break:\n"
"                uiLeftSrcSampleInd = max( uint(iFirstDepthBreakToTheLeftInd + 1), uiInitialSample0Ind );\n"
"            }\n"
"\n"
"            // Find right interpolation source using symmetric method\n"
"            {\n"
"                // We need to find first depth break starting from iRightSrcSampleInd and\n"
"                // going to the right up to the uiInitialSample1Ind\n"
"                uiRightSrcSampleInd = uiSampleInd;\n"
"                int iFlagPackOrder = int(uiRightSrcSampleInd / 32u );\n"
"                int iFlagOrderInPack = int(uiRightSrcSampleInd % 32u);\n"
"                uint uiFlagPack = uiPackedCamSpaceDiffFlags[iFlagPackOrder];\n"
"                // We need to find first unset flag in the current flag pack\n"
"                // starting from iFlagOrderInPack position and up to the 31st bit\n"
"                // Set all lower order bits to 1 so that they are skipped during\n"
"                // the test:\n"
"                if( iFlagOrderInPack > 0 )\n"
"                    uiFlagPack |= uint( (1 << iFlagOrderInPack)-1 );\n"
"                // Find first zero flag:\n"
"                int iFirstUnsetFlagPos = firstbitlow( uint(~uiFlagPack) );\n"
"                if( !(0 <= iFirstUnsetFlagPos && iFirstUnsetFlagPos < 32) )\n"
"                    iFirstUnsetFlagPos = 32;\n"
"                uiRightSrcSampleInd += uint(iFirstUnsetFlagPos - iFlagOrderInPack);\n"
"\n"
"#if INITIAL_SAMPLE_STEP > 32\n"
"                // Check the remaining full flag packs\n"
"                iFlagPackOrder++;\n"
"                while( iFlagPackOrder < int(NUM_PACKED_FLAGS) && iFirstUnsetFlagPos == 32 )\n"
"                {\n"
"                    uiFlagPack = uiPackedCamSpaceDiffFlags[iFlagPackOrder];\n"
"                    iFirstUnsetFlagPos = firstbitlow( uint(~uiFlagPack) );\n"
"                    if( !(0 <= iFirstUnsetFlagPos && iFirstUnsetFlagPos < 32) )\n"
"                        iFirstUnsetFlagPos = 32;\n"
"                    uiRightSrcSampleInd += uint(iFirstUnsetFlagPos);\n"
"                    iFlagPackOrder++;\n"
"                }\n"
"#endif\n"
"                uiRightSrcSampleInd = min(uiRightSrcSampleInd, uiInitialSample1Ind);\n"
"            }\n"
"        }\n"
"\n"
"        // If at least one interpolation source is the same as the sample itself, the\n"
"        // sample is ray marching sample and is interpolated from itself:\n"
"        if(uiLeftSrcSampleInd == uiSampleInd || uiRightSrcSampleInd == uiSampleInd )\n"
"            uiLeftSrcSampleInd = uiRightSrcSampleInd = uiSampleInd;\n"
"    }\n"
"\n"
"    g_rwtex2DInterpolationSource[ int2(uiGlobalSampleInd, uiSliceInd) ] = uint2(uiGroupStartGlobalInd + uiLeftSrcSampleInd, uiGroupStartGlobalInd + uiRightSrcSampleInd);\n"
"}\n"
