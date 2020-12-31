#ifndef _HTFIR2H
#define _HTFIR2H

// fir lowpass real 7.8M
float FIR2[1025] = {
    0.000000050349,
    0.000000094754,
    0.000000089373,
    0.000000025586,
   -0.000000068960,
   -0.000000140583,
   -0.000000139027,
   -0.000000050558,
    0.000000086162,
    0.000000194854,
    0.000000203008,
    0.000000088104,
   -0.000000099819,
   -0.000000257351,
   -0.000000283267,
   -0.000000141386,
    0.000000107250,
    0.000000327407,
    0.000000381645,
    0.000000213880,
   -0.000000105191,
   -0.000000403809,
   -0.000000499787,
   -0.000000309338,
    0.000000089757,
    0.000000484706,
    0.000000639034,
    0.000000431730,
   -0.000000056420,
   -0.000000567517,
   -0.000000800311,
   -0.000000585169,
    0.000000000000,
    0.000000648839,
    0.000000983996,
    0.000000773817,
    0.000000085327,
   -0.000000724358,
   -0.000001189789,
   -0.000001001777,
   -0.000000206004,
    0.000000788772,
    0.000001416565,
    0.000001272953,
    0.000000369051,
   -0.000000835722,
   -0.000001662221,
   -0.000001590907,
   -0.000000581986,
    0.000000857736,
    0.000001923530,
    0.000001958678,
    0.000000852740,
   -0.000000846194,
   -0.000002195978,
   -0.000002378603,
   -0.000001189529,
    0.000000791313,
    0.000002473623,
    0.000002852103,
    0.000001600704,
   -0.000000682154,
   -0.000002748950,
   -0.000003379463,
   -0.000002094572,
    0.000000506663,
    0.000003012735,
    0.000003959605,
    0.000002679187,
   -0.000000251738,
   -0.000003253941,
   -0.000004589839,
   -0.000003362106,
   -0.000000096664,
    0.000003459616,
    0.000005265619,
    0.000004150125,
    0.000000553384,
   -0.000003614830,
   -0.000005980297,
   -0.000005048979,
   -0.000001133879,
    0.000003702640,
    0.000006724881,
    0.000006063020,
    0.000001853993,
   -0.000003704092,
   -0.000007487800,
   -0.000007194872,
   -0.000002729689,
    0.000003598263,
    0.000008254690,
    0.000008445068,
    0.000003776731,
   -0.000003362349,
   -0.000009008203,
   -0.000009811671,
   -0.000005010327,
    0.000002971812,
    0.000009727839,
    0.000011289887,
    0.000006444725,
   -0.000002400569,
   -0.000010389825,
   -0.000012871675,
   -0.000008092764,
    0.000001621255,
    0.000010967029,
    0.000014545362,
    0.000009965393,
   -0.000000605537,
   -0.000011428933,
   -0.000016295269,
   -0.000012071152,
   -0.000000675504,
    0.000011741656,
    0.000018101360,
    0.000014415617,
    0.000002250922,
   -0.000011868055,
   -0.000019938917,
   -0.000017000833,
   -0.000004149395,
    0.000011767883,
    0.000021778254,
    0.000019824716,
    0.000006398649,
   -0.000011398039,
   -0.000023584485,
   -0.000022880460,
   -0.000009024810,
    0.000010712895,
    0.000025317346,
    0.000026155935,
    0.000012051711,
   -0.000009664710,
   -0.000026931087,
   -0.000029633102,
   -0.000015500135,
    0.000008204138,
    0.000028374443,
    0.000033287447,
    0.000019387010,
   -0.000006280833,
   -0.000029590694,
   -0.000037087453,
   -0.000023724572,
    0.000003844139,
    0.000030517825,
    0.000040994114,
    0.000028519486,
   -0.000000843883,
   -0.000031088791,
   -0.000044960519,
   -0.000033771956,
   -0.000002768740,
    0.000031231894,
    0.000048931496,
    0.000039474822,
    0.000007040204,
   -0.000030871287,
   -0.000052843362,
   -0.000045612663,
   -0.000012013592,
    0.000029927600,
    0.000056623757,
    0.000052160919,
    0.000017727475,
   -0.000028318697,
   -0.000060191603,
   -0.000059085054,
   -0.000024214715,
    0.000025960569,
    0.000063457194,
    0.000066339760,
    0.000031501218,
   -0.000022768357,
   -0.000066322415,
   -0.000073868246,
   -0.000039604645,
    0.000018657507,
    0.000068681123,
    0.000081601599,
    0.000048533099,
   -0.000013545056,
   -0.000070419683,
   -0.000089458263,
   -0.000058283802,
    0.000007351038,
    0.000071417677,
    0.000097343635,
    0.000068841774,
   -0.000000000000,
   -0.000071548788,
   -0.000105149803,
   -0.000080178551,
   -0.000008577368,
    0.000070681866,
    0.000112755454,
    0.000092250945,
    0.000018442526,
   -0.000068682171,
   -0.000120025943,
   -0.000104999883,
   -0.000029647288,
    0.000065412813,
    0.000126813565,
    0.000118349331,
    0.000042231985,
   -0.000060736356,
   -0.000132958034,
   -0.000132205346,
   -0.000056223576,
    0.000054516609,
    0.000138287169,
    0.000146455267,
    0.000071633752,
   -0.000046620579,
   -0.000142617829,
   -0.000160967065,
   -0.000088457047,
    0.000036920583,
    0.000145757065,
    0.000175588897,
    0.000106668984,
   -0.000025296495,
   -0.000147503527,
   -0.000190148851,
   -0.000126224286,
    0.000011638127,
    0.000147649116,
    0.000204454946,
    0.000147055167,
    0.000004152293,
   -0.000145980875,
   -0.000218295363,
   -0.000169069756,
   -0.000022157557,
    0.000142283129,
    0.000231438964,
    0.000192150661,
    0.000042442858,
   -0.000136339851,
   -0.000243636079,
   -0.000216153716,
   -0.000065053144,
    0.000127937259,
    0.000254619610,
    0.000240906932,
    0.000090010470,
   -0.000116866619,
   -0.000264106422,
   -0.000266209693,
   -0.000117311386,
    0.000102927240,
    0.000271799069,
    0.000291832206,
    0.000146924394,
   -0.000085929638,
   -0.000277387820,
   -0.000317515252,
   -0.000178787501,
    0.000065698851,
    0.000280553013,
    0.000342970241,
    0.000212805916,
   -0.000042077866,
   -0.000280967714,
   -0.000367879613,
   -0.000248849917,
    0.000014931133,
    0.000278300683,
    0.000391897581,
    0.000286752927,
    0.000015851860,
   -0.000272219622,
   -0.000414651249,
   -0.000326309838,
   -0.000050353005,
    0.000262394709,
    0.000435742113,
    0.000367275601,
    0.000088622006,
   -0.000248502365,
   -0.000454747936,
   -0.000409364144,
   -0.000130672876,
    0.000230229259,
    0.000471225022,
    0.000452247610,
    0.000176480531,
   -0.000207276502,
   -0.000484710872,
   -0.000495555973,
   -0.000225977528,
    0.000179364002,
    0.000494727221,
    0.000538877038,
    0.000279050988,
   -0.000146234951,
   -0.000500783437,
   -0.000581756846,
   -0.000335539740,
    0.000107660385,
    0.000502380277,
    0.000623700507,
    0.000395231729,
   -0.000063443803,
   -0.000499013959,
   -0.000664173459,
   -0.000457861717,
    0.000013425769,
    0.000490180543,
    0.000702603162,
    0.000523109326,
    0.000042511516,
   -0.000475380574,
   -0.000738381230,
   -0.000590597431,
   -0.000104439748,
    0.000454123951,
    0.000770865976,
    0.000659890950,
    0.000172380183,
   -0.000425934984,
   -0.000799385380,
   -0.000730496036,
   -0.000246299486,
    0.000390357594,
    0.000823240429,
    0.000801859686,
    0.000326105807,
   -0.000346960600,
   -0.000841708821,
   -0.000873369787,
   -0.000411645125,
    0.000295343040,
    0.000854048983,
    0.000944355577,
    0.000502697888,
   -0.000235139478,
   -0.000859504365,
   -0.001014088528,
   -0.000598976003,
    0.000166025228,
    0.000857307944,
    0.001081783620,
    0.000700120170,
   -0.000087721442,
   -0.000846686891,
   -0.001146600984,
   -0.000805697611,
    0.000000000000,
    0.000826867317,
    0.001207647851,
    0.000915200175,
    0.000097311862,
   -0.000797079020,
   -0.001263980780,
   -0.001028042843,
   -0.000204327241,
    0.000756560141,
    0.001314608059,
    0.001143562600,
    0.000321095638,
   -0.000704561636,
   -0.001358492205,
   -0.001261017657,
   -0.000447599460,
    0.000640351424,
    0.001394552453,
    0.001379586972,
    0.000583751016,
   -0.000563218115,
   -0.001421667086,
   -0.001498370015,
   -0.000729390046,
    0.000472474142,
    0.001438675449,
    0.001616386676,
    0.000884281838,
   -0.000367458154,
   -0.001444379447,
   -0.001732577204,
   -0.001048115969,
    0.000247536470,
    0.001437544290,
    0.001845802022,
    0.001220505704,
   -0.000112103368,
   -0.001416898183,
   -0.001954841231,
   -0.001400988083,
   -0.000039420033,
    0.001381130610,
    0.002058393530,
    0.001589024716,
    0.000207588629,
   -0.001328888768,
   -0.002155074260,
   -0.001784003306,
   -0.000392938283,
    0.001258771592,
    0.002243412139,
    0.001985239888,
    0.000595993369,
   -0.001169320678,
   -0.002321844176,
   -0.002191981811,
   -0.000817277473,
    0.001059007194,
    0.002388708076,
    0.002403411426,
    0.001057328035,
   -0.000926213628,
   -0.002442231239,
   -0.002618650480,
   -0.001316715925,
    0.000769208823,
    0.002480515184,
    0.002836765181,
    0.001596071273,
   -0.000586114246,
   -0.002501513783,
   -0.003056771902,
   -0.001896117370,
    0.000374858716,
    0.002503003172,
    0.003277643481,
    0.002217715095,
   -0.000133117726,
   -0.002482540333,
   -0.003498316076,
   -0.002561921346,
   -0.000141768021,
    0.002437406149,
    0.003717696500,
    0.002930066390,
    0.000452902560,
   -0.002364526931,
   -0.003934669993,
   -0.003323857288,
   -0.000803984270,
    0.002260365674,
    0.004148108350,
    0.003745517916,
    0.001199511702,
   -0.002120770003,
   -0.004356878335,
   -0.004197981514,
   -0.001645070716,
    0.001940757029,
    0.004559850303,
    0.004685160265,
    0.002147741818,
   -0.001714204236,
   -0.004755906942,
   -0.005212330753,
   -0.002716689632,
    0.001433396925,
    0.004943952065,
    0.005786698470,
    0.003364036010,
   -0.001088350523,
   -0.005122919348,
   -0.006418247642,
   -0.004106188957,
    0.000665767981,
    0.005291780938,
    0.007121061711,
    0.004965930626,
   -0.000147383462,
   -0.005449555847,
   -0.007915451984,
   -0.005975823095,
   -0.000492771593,
    0.005595318037,
    0.008831540548,
    0.007184016438,
    0.001293093743,
   -0.005728204120,
   -0.009915610604,
   -0.008664700434,
   -0.002313046104,
    0.005847420591,
    0.011242093783,
    0.010538199918,
    0.003650092098,
   -0.005952250525,
   -0.012938056544,
   -0.013012986513,
   -0.005475983528,
    0.006042059659,
    0.015238605145,
    0.016483697783,
    0.008124279233,
   -0.006116301799,
   -0.018630923786,
   -0.021797177385,
   -0.012339435185,
    0.006174523499,
    0.024312557720,
    0.031156993405,
    0.020190130004,
   -0.006216367958,
   -0.036220921934,
   -0.052645997477,
   -0.040366539358,
    0.006241578092,
    0.079298809648,
    0.159019490059,
    0.220612104543,
    0.243749951585,
    0.220612104543,
    0.159019490059,
    0.079298809648,
    0.006241578092,
   -0.040366539358,
   -0.052645997477,
   -0.036220921934,
   -0.006216367958,
    0.020190130004,
    0.031156993405,
    0.024312557720,
    0.006174523499,
   -0.012339435185,
   -0.021797177385,
   -0.018630923786,
   -0.006116301799,
    0.008124279233,
    0.016483697783,
    0.015238605145,
    0.006042059659,
   -0.005475983528,
   -0.013012986513,
   -0.012938056544,
   -0.005952250525,
    0.003650092098,
    0.010538199918,
    0.011242093783,
    0.005847420591,
   -0.002313046104,
   -0.008664700434,
   -0.009915610604,
   -0.005728204120,
    0.001293093743,
    0.007184016438,
    0.008831540548,
    0.005595318037,
   -0.000492771593,
   -0.005975823095,
   -0.007915451984,
   -0.005449555847,
   -0.000147383462,
    0.004965930626,
    0.007121061711,
    0.005291780938,
    0.000665767981,
   -0.004106188957,
   -0.006418247642,
   -0.005122919348,
   -0.001088350523,
    0.003364036010,
    0.005786698470,
    0.004943952065,
    0.001433396925,
   -0.002716689632,
   -0.005212330753,
   -0.004755906942,
   -0.001714204236,
    0.002147741818,
    0.004685160265,
    0.004559850303,
    0.001940757029,
   -0.001645070716,
   -0.004197981514,
   -0.004356878335,
   -0.002120770003,
    0.001199511702,
    0.003745517916,
    0.004148108350,
    0.002260365674,
   -0.000803984270,
   -0.003323857288,
   -0.003934669993,
   -0.002364526931,
    0.000452902560,
    0.002930066390,
    0.003717696500,
    0.002437406149,
   -0.000141768021,
   -0.002561921346,
   -0.003498316076,
   -0.002482540333,
   -0.000133117726,
    0.002217715095,
    0.003277643481,
    0.002503003172,
    0.000374858716,
   -0.001896117370,
   -0.003056771902,
   -0.002501513783,
   -0.000586114246,
    0.001596071273,
    0.002836765181,
    0.002480515184,
    0.000769208823,
   -0.001316715925,
   -0.002618650480,
   -0.002442231239,
   -0.000926213628,
    0.001057328035,
    0.002403411426,
    0.002388708076,
    0.001059007194,
   -0.000817277473,
   -0.002191981811,
   -0.002321844176,
   -0.001169320678,
    0.000595993369,
    0.001985239888,
    0.002243412139,
    0.001258771592,
   -0.000392938283,
   -0.001784003306,
   -0.002155074260,
   -0.001328888768,
    0.000207588629,
    0.001589024716,
    0.002058393530,
    0.001381130610,
   -0.000039420033,
   -0.001400988083,
   -0.001954841231,
   -0.001416898183,
   -0.000112103368,
    0.001220505704,
    0.001845802022,
    0.001437544290,
    0.000247536470,
   -0.001048115969,
   -0.001732577204,
   -0.001444379447,
   -0.000367458154,
    0.000884281838,
    0.001616386676,
    0.001438675449,
    0.000472474142,
   -0.000729390046,
   -0.001498370015,
   -0.001421667086,
   -0.000563218115,
    0.000583751016,
    0.001379586972,
    0.001394552453,
    0.000640351424,
   -0.000447599460,
   -0.001261017657,
   -0.001358492205,
   -0.000704561636,
    0.000321095638,
    0.001143562600,
    0.001314608059,
    0.000756560141,
   -0.000204327241,
   -0.001028042843,
   -0.001263980780,
   -0.000797079020,
    0.000097311862,
    0.000915200175,
    0.001207647851,
    0.000826867317,
    0.000000000000,
   -0.000805697611,
   -0.001146600984,
   -0.000846686891,
   -0.000087721442,
    0.000700120170,
    0.001081783620,
    0.000857307944,
    0.000166025228,
   -0.000598976003,
   -0.001014088528,
   -0.000859504365,
   -0.000235139478,
    0.000502697888,
    0.000944355577,
    0.000854048983,
    0.000295343040,
   -0.000411645125,
   -0.000873369787,
   -0.000841708821,
   -0.000346960600,
    0.000326105807,
    0.000801859686,
    0.000823240429,
    0.000390357594,
   -0.000246299486,
   -0.000730496036,
   -0.000799385380,
   -0.000425934984,
    0.000172380183,
    0.000659890950,
    0.000770865976,
    0.000454123951,
   -0.000104439748,
   -0.000590597431,
   -0.000738381230,
   -0.000475380574,
    0.000042511516,
    0.000523109326,
    0.000702603162,
    0.000490180543,
    0.000013425769,
   -0.000457861717,
   -0.000664173459,
   -0.000499013959,
   -0.000063443803,
    0.000395231729,
    0.000623700507,
    0.000502380277,
    0.000107660385,
   -0.000335539740,
   -0.000581756846,
   -0.000500783437,
   -0.000146234951,
    0.000279050988,
    0.000538877038,
    0.000494727221,
    0.000179364002,
   -0.000225977528,
   -0.000495555973,
   -0.000484710872,
   -0.000207276502,
    0.000176480531,
    0.000452247610,
    0.000471225022,
    0.000230229259,
   -0.000130672876,
   -0.000409364144,
   -0.000454747936,
   -0.000248502365,
    0.000088622006,
    0.000367275601,
    0.000435742113,
    0.000262394709,
   -0.000050353005,
   -0.000326309838,
   -0.000414651249,
   -0.000272219622,
    0.000015851860,
    0.000286752927,
    0.000391897581,
    0.000278300683,
    0.000014931133,
   -0.000248849917,
   -0.000367879613,
   -0.000280967714,
   -0.000042077866,
    0.000212805916,
    0.000342970241,
    0.000280553013,
    0.000065698851,
   -0.000178787501,
   -0.000317515252,
   -0.000277387820,
   -0.000085929638,
    0.000146924394,
    0.000291832206,
    0.000271799069,
    0.000102927240,
   -0.000117311386,
   -0.000266209693,
   -0.000264106422,
   -0.000116866619,
    0.000090010470,
    0.000240906932,
    0.000254619610,
    0.000127937259,
   -0.000065053144,
   -0.000216153716,
   -0.000243636079,
   -0.000136339851,
    0.000042442858,
    0.000192150661,
    0.000231438964,
    0.000142283129,
   -0.000022157557,
   -0.000169069756,
   -0.000218295363,
   -0.000145980875,
    0.000004152293,
    0.000147055167,
    0.000204454946,
    0.000147649116,
    0.000011638127,
   -0.000126224286,
   -0.000190148851,
   -0.000147503527,
   -0.000025296495,
    0.000106668984,
    0.000175588897,
    0.000145757065,
    0.000036920583,
   -0.000088457047,
   -0.000160967065,
   -0.000142617829,
   -0.000046620579,
    0.000071633752,
    0.000146455267,
    0.000138287169,
    0.000054516609,
   -0.000056223576,
   -0.000132205346,
   -0.000132958034,
   -0.000060736356,
    0.000042231985,
    0.000118349331,
    0.000126813565,
    0.000065412813,
   -0.000029647288,
   -0.000104999883,
   -0.000120025943,
   -0.000068682171,
    0.000018442526,
    0.000092250945,
    0.000112755454,
    0.000070681866,
   -0.000008577368,
   -0.000080178551,
   -0.000105149803,
   -0.000071548788,
   -0.000000000000,
    0.000068841774,
    0.000097343635,
    0.000071417677,
    0.000007351038,
   -0.000058283802,
   -0.000089458263,
   -0.000070419683,
   -0.000013545056,
    0.000048533099,
    0.000081601599,
    0.000068681123,
    0.000018657507,
   -0.000039604645,
   -0.000073868246,
   -0.000066322415,
   -0.000022768357,
    0.000031501218,
    0.000066339760,
    0.000063457194,
    0.000025960569,
   -0.000024214715,
   -0.000059085054,
   -0.000060191603,
   -0.000028318697,
    0.000017727475,
    0.000052160919,
    0.000056623757,
    0.000029927600,
   -0.000012013592,
   -0.000045612663,
   -0.000052843362,
   -0.000030871287,
    0.000007040204,
    0.000039474822,
    0.000048931496,
    0.000031231894,
   -0.000002768740,
   -0.000033771956,
   -0.000044960519,
   -0.000031088791,
   -0.000000843883,
    0.000028519486,
    0.000040994114,
    0.000030517825,
    0.000003844139,
   -0.000023724572,
   -0.000037087453,
   -0.000029590694,
   -0.000006280833,
    0.000019387010,
    0.000033287447,
    0.000028374443,
    0.000008204138,
   -0.000015500135,
   -0.000029633102,
   -0.000026931087,
   -0.000009664710,
    0.000012051711,
    0.000026155935,
    0.000025317346,
    0.000010712895,
   -0.000009024810,
   -0.000022880460,
   -0.000023584485,
   -0.000011398039,
    0.000006398649,
    0.000019824716,
    0.000021778254,
    0.000011767883,
   -0.000004149395,
   -0.000017000833,
   -0.000019938917,
   -0.000011868055,
    0.000002250922,
    0.000014415617,
    0.000018101360,
    0.000011741656,
   -0.000000675504,
   -0.000012071152,
   -0.000016295269,
   -0.000011428933,
   -0.000000605537,
    0.000009965393,
    0.000014545362,
    0.000010967029,
    0.000001621255,
   -0.000008092764,
   -0.000012871675,
   -0.000010389825,
   -0.000002400569,
    0.000006444725,
    0.000011289887,
    0.000009727839,
    0.000002971812,
   -0.000005010327,
   -0.000009811671,
   -0.000009008203,
   -0.000003362349,
    0.000003776731,
    0.000008445068,
    0.000008254690,
    0.000003598263,
   -0.000002729689,
   -0.000007194872,
   -0.000007487800,
   -0.000003704092,
    0.000001853993,
    0.000006063020,
    0.000006724881,
    0.000003702640,
   -0.000001133879,
   -0.000005048979,
   -0.000005980297,
   -0.000003614830,
    0.000000553384,
    0.000004150125,
    0.000005265619,
    0.000003459616,
   -0.000000096664,
   -0.000003362106,
   -0.000004589839,
   -0.000003253941,
   -0.000000251738,
    0.000002679187,
    0.000003959605,
    0.000003012735,
    0.000000506663,
   -0.000002094572,
   -0.000003379463,
   -0.000002748950,
   -0.000000682154,
    0.000001600704,
    0.000002852103,
    0.000002473623,
    0.000000791313,
   -0.000001189529,
   -0.000002378603,
   -0.000002195978,
   -0.000000846194,
    0.000000852740,
    0.000001958678,
    0.000001923530,
    0.000000857736,
   -0.000000581986,
   -0.000001590907,
   -0.000001662221,
   -0.000000835722,
    0.000000369051,
    0.000001272953,
    0.000001416565,
    0.000000788772,
   -0.000000206004,
   -0.000001001777,
   -0.000001189789,
   -0.000000724358,
    0.000000085327,
    0.000000773817,
    0.000000983996,
    0.000000648839,
    0.000000000000,
   -0.000000585169,
   -0.000000800311,
   -0.000000567517,
   -0.000000056420,
    0.000000431730,
    0.000000639034,
    0.000000484706,
    0.000000089757,
   -0.000000309338,
   -0.000000499787,
   -0.000000403809,
   -0.000000105191,
    0.000000213880,
    0.000000381645,
    0.000000327407,
    0.000000107250,
   -0.000000141386,
   -0.000000283267,
   -0.000000257351,
   -0.000000099819,
    0.000000088104,
    0.000000203008,
    0.000000194854,
    0.000000086162,
   -0.000000050558,
   -0.000000139027,
   -0.000000140583,
   -0.000000068960,
    0.000000025586,
    0.000000089373,
    0.000000094754,
    0.000000050349
};
#endif