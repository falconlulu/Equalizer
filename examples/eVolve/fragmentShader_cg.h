static const std::string fragmentShader_cg = "/* Copyright (c) 2007       Maxim Makhinya\n    All rights reserved. */\n \n void main(\n     uniform sampler3D volume,\n     uniform sampler2D preInt,\n \n     uniform half      shininess,\n \n     float3  frontP  : TEXCOORD0,\n     float3  backP   : TEXCOORD1,\n \n out float4   oColor  : COLOR \n \n )\n {\n     half4 lookupSF = tex3D( volume, frontP );\n     half4 lookupSB = tex3D( volume, backP  );\n     half4 preInt_  = tex2D( preInt, half2(lookupSF.a, lookupSB.a) );\n \n // lighting\n     half3 normalSF = lookupSF.rgb*2.0 - 1.0;\n     half3 normalSB = lookupSB.rgb*2.0 - 1.0;\n \n     half3 normal = normalize( normalSF+normalSB );\n \n     half3 L = (glstate.light[0].position).xyz;\n \n     half3 halfVector = normalize(L);\n \n     half diffuse  = max( dot( glstate.light[0].position.xyz, normal ), 0.0 );\n \n     half specular = pow( max( dot( halfVector, normal), 0.0 ), shininess ); \n \n     oColor = float4(glstate.light[0].ambient.rgb  * preInt_.rgb +\n                     glstate.light[0].diffuse.rgb  * preInt_.rgb * diffuse +\n                     glstate.light[0].specular.rgb * preInt_.rgb * specular,\n                     preInt_.a);\n \n }\n \n ";
