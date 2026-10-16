# Compatibility

Results of running every 68k application found in the `Software/` folder
(imported into a scratch library) headless: each runs for up to 60 million
instructions or 45 seconds, with no input.  Measured 2026-10-16.

A headless run says how far an application gets on its own, not whether it
is usable: "waiting for input" means it reached its event loop (a window,
dialog or splash screen and nothing more to do until someone clicks or
types); "ran and quit" covers installers and updaters that find nothing to
do, and applications that decline to run (needing QuickTime, a CD, more
memory …) as well as ones that finish normally; "running at the limit" means
it was still busy (often an animation, a game loop, or a wait for something
that never comes).

| Result | Applications |
|---|---|
| Runs, waiting for input | 453 |
| Ran and quit | 144 |
| Running at the limit | 54 |
| Stopped: other | 8 |
| Too slow (45 s timeout) | 83 |
| **Total** | **742** |

Duplicates (the same application found in several archives) are listed once.

## Runs, waiting for input (453)

| Application | From | 
|---|---|
| **Read Me** S.A.J. | shareware games s |
| Adobe Dimensions® 3.0 Installer | Adobe Photoshop 5.toast  |
| AdobePS Installer | Adobe Photoshop 5.toast  |
| Aladdin Expander™ 5.5 Installer | stuffit expander 55 |
| Alius Installer | Adobe Photoshop 5.toast  |
| Apeiron Installer | Apeiron 1 0 3 |
| Apple Hardware Addendum | Adobe Photoshop 5.toast  |
| AppleWorks 5.0.4 Updater | AppleWorks5.0.4 |
| Bachman 1.0.1u | Cool Games One |
| BBEdit 2.1.3 | bbedit 2.1.3 |
| Bestellformular | shareware games s |
| Bolo 0.99.7 | bolo-0997 |
| Brochure | shareware games s |
| Browser | gregs-browser-27 |
| Chromassage Demo Installer | Adobe Photoshop 5.toast  |
| Chromassage™Demo | Adobe Photoshop 5.toast  |
| CoCo 3.0 install | Adobe Photoshop 5.toast  |
| Compact Pro User's Guide | CompactPro1.50 |
| Continuum | Continuum |
| Cribbage 2.5 | Cool Games Two |
| CritterEditor 1.0.4M | crystalquest 2 2 5m |
| Crystal Quest | crystalquest 2 2 5m |
| Crystal Quest Sample | crystalquest 2 2 5m |
| Demo manual | shareware games s |
| DiskDup+ v2.9.2 | DiskDup2.9.2 |
| Dizzy Tryout Installer | Adobe Photoshop 5.toast  |
| Doppelkopf for Beginners 1.1 | shareware games s |
| Downloader 5.0.5 | Acrobat Reader 301 |
| Dream 1.6.1->1.6.2 | shareware games s |
| Epson Hardware Addendum | Adobe Photoshop 5.toast  |
| Equation Editor | Microsoft Word 5.0 sv |
| Euchre 5.1 | Cool Games Two |
| Fetch 2.1.2 | fetch |
| Fetch 3.0.3 | fetch |
| Fetch 4.0.3 | fetch |
| furbo filters installer | Adobe Photoshop 5.toast  |
| GenesisDemo | Adobe Photoshop 5.toast  |
| Glider 4.06 | Glider 42 |
| Glider 4.09 | Glider 42 |
| Glider+ 3.1.2 | Glider312 |
| HemiRoids 1.0.8 | hemiroids 1.0.8 |
| Hi Rez Demo Projector | Journeyman.toast  |
| HoloDozoDemo | Adobe Photoshop 5.toast  |
| HotTEXT Installer | Adobe Photoshop 5.toast  |
| How to register | shareware games s |
| HP  Hardware Addendum | Adobe Photoshop 5.toast  |
| HVS JPEG 2.0 Installer | Adobe Photoshop 5.toast  |
| IconQuest 1.21 | Cool Games Two |
| IE 4.01 128 Bit Installer | IE401-Full-Fat-128-img |
| IE 4.01 Full Fat Installer | IE401-Full-Fat-128-img |
| Infini-D_ 4.5 Installer | infini-d 4.5 |
| Install Adobe Illustrator® 7.0 | Adobe Photoshop 5.toast  |
| Install Adobe Photoshop® | Adobe Photoshop 5.toast  |
| Install Adobe Premiere™ Demo | Adobe Photoshop 5.toast  |
| Install AE 3.1 Tryout | Adobe Photoshop 5.toast  |
| Install AppleWorks | AppleWorks5.0.4 |
| Install BladePro 2.0.8 | Adobe Photoshop 5.toast  |
| Install ClarisWorks | ClarisWorks 3.0 Mac US |
| Install Harmony for Photoshop | Adobe Photoshop 5.toast  |
| Install HexWrench 1.0.1 Demo | Adobe Photoshop 5.toast  |
| Install OLE Plug-in | Acrobat Reader 301 |
| Install PM6.5 Tryout | Adobe Photoshop 5.toast  |
| Install ScanPrepPro | Adobe Photoshop 5.toast  |
| Install Series 1 cMulti Demo | Adobe Photoshop 5.toast  |
| Install Series 2 Three-D Demo | Adobe Photoshop 5.toast  |
| Install Series 3 Screens Demo | Adobe Photoshop 5.toast  |
| Install Shadow Demo | Adobe Photoshop 5.toast  |
| Install StudioPro™ 2.5 | Strata Studio Pro 2.5 |
| Install Test Strip 2.0.2 | Adobe Photoshop 5.toast  |
| Installer | Acrobat Reader 301 |
| Installer | Adobe Photoshop 5.toast  |
| Installer | HyperCard241 |
| Intellihance™ 3.0.2 | Adobe Photoshop 5.toast  |
| Internet Config | fetch |
| Internet Explorer | Internet Explorer 5.1.7.smi  |
| Iso! 1.0.1 | Cool Games Two |
| Journeyman 16MB | Journeyman.toast  |
| Journeyman 5MB | Journeyman.toast  |
| Journeyman 8MB | Journeyman.toast  |
| KBNK Generator | shareware games s |
| KQKR Generator | shareware games s |
| Laurie's e-card | shareware games s |
| Level Mover | Diamonds 1.5 |
| Mac OS Install | HyperCard241 |
| Mac Paint 2.0 | macpaint2.0.sea  |
| MacDraw | MacDrawv1.9.8.image  |
| Macintosh System Errors 7.5 | Macintosh System Errors 7.5 |
| Macman 2.0 | Cool Games One |
| MacPlaymate II | MacPlaymate II |
| MacroMind Director 3.1.1 | MacroMediaDirector3.1 |
| MacroMind Player | MacroMediaDirector3.1 |
| MacSokoban 2.0 | Cool Games One |
| MacSokoban 2.1 | shareware games s |
| MacStardate 1.0.1 | shareware games s |
| Mad Cow Roulette | shareware games s |
| Maelstrom Installer | Maelstrom 1.4.3 Installer |
| Maelstrom Installer | maelstrom-142 |
| Mask Pro™ 1.0.1 | Adobe Photoshop 5.toast  |
| Microsoft Setup | Excel 5.0 English |
| Microsoft System Information | Excel 5.0 English |
| Microsoft Word | Microsoft Word 5.0 sv |
| MineField | Cool Games One |
| Monkey Farm Software | shareware games s |
| More Info about Stargazer | shareware games s |
| MoviePlay | Adobe Photoshop 5.toast  |
| NCSA Mosaic 1.0.3 | NCSAMosaic103 |
| Netscape 1.0N | Netscape 1.0N |
| netscape3.04_US | netscape3.04 128 |
| Order Form | shareware games s |
| Out of this World™ "mv" ->1.0.3 | out of this world |
| Out of this World™ -> 1.0.3 | out of this world |
| PenTools Manual | Adobe Photoshop 5.toast  |
| PhotoFrame™1.0 | Adobe Photoshop 5.toast  |
| Photoshop50-SDKr1Mac.install | Adobe Photoshop 5.toast  |
| PhotoTools™ 2.0 Sampler | Adobe Photoshop 5.toast  |
| Planets v1.0.3 Updater | shareware games s |
| PlayerPRO 4.5 (FAT) | PlayerPRO 4.5 FAT Eng |
| Polyominoes 3.0 | Cool Games Two |
| Portfolio Browser | Adobe Photoshop 5.toast  |
| Prince of Persia 2 | PoP Collection Limited.cdr  |
| QuickHelp™ 4.3.1 | Adobe Photoshop 5.toast  |
| READ ME - SS Warlock Manual | spaceship-warlock-1996 |
| Read me first ! | shareware games s |
| Read me First! | Cool Games Two |
| Read Me! | shareware games s |
| Reader 3.01 Installer | Acrobat Reader 301 |
| Reader 3.01 Installer | Adobe Photoshop 5.toast  |
| Reader 3.01 Installer | HyperCard241 |
| Reader 3.02 Installer | PoP Collection Limited.cdr  |
| ReadMe | BadDay.toast  |
| Readme Demo (3.0:1.1) | shareware games s |
| Readme Shareware | shareware games s |
| Register | gregs-browser-27 |
| Register | shareware games s |
| Register (Kagi) | shareware games s |
| Register C4 | shareware games s |
| Register Curry Software | shareware games s |
| Register Me | shareware games s |
| Register Me (Space Junkie 1.2) | shareware games s |
| Register Safari | shareware games s |
| Register Save the World™ | shareware games s |
| Register Scrungle | shareware games s |
| Register SCS | shareware games s |
| Register SCS Snake | shareware games s |
| Register Snavely | shareware games s |
| Register Sound Studio | shareware games s |
| Register STRiKE Korea 97 | shareware games s |
| Register via Kagi | shareware games s |
| Register YLS | shareware games s |
| Register ∑ Chess 5 Pro | shareware games s |
| Registration (open me!) | shareware games s |
| Registration Form | TetrisMax 2.8 |
| Rescue! 1.6 | shareware games s |
| RoboWar 2.3 | Cool Games Two |
| RoboWar.sea | Cool Games Two |
| Rock Scissors Paper | shareware games s |
| Safecracker | shareware games s |
| Same Game 1.0.7 | shareware games s |
| Same Game GÉ-1.0.8 | shareware games s |
| Sample Game | shareware games s |
| Save Princeton 1.9 | shareware games s |
| Save the World™ | shareware games s |
| ScanTasticCentral™ | Adobe Photoshop 5.toast  |
| Scarab of RA 1.4 | scarab of ra |
| Scarab of RA 1.4 | shareware games s |
| ScenarioMaker 1.6.2->1.7.1 | shareware games s |
| ScenarioMaker 3D DR1 | shareware games s |
| Scholastic Challenge | shareware games s |
| Scopa 2.0 | shareware games s |
| Score A Million 1.0 | shareware games s |
| Score A Million Game Installer | shareware games s |
| Scrambler Doc | shareware games s |
| ScrapIt Pro | shareware games s |
| ScrapIt Pro Documentation | shareware games s |
| Screen Rodeo | shareware games s |
| Scruffy II | shareware games s |
| Scrungle | shareware games s |
| Scrungle Read Me | shareware games s |
| SCS Connect Four v1.0.1 | shareware games s |
| SCS Mines 1.0 | shareware games s |
| SCS Mines 1.0 [FAT] | shareware games s |
| SCS Snake 1.3.0 [FAT] | shareware games s |
| SCS Snake 1.3.1 [FAT] | shareware games s |
| Sea Wolf Demo | shareware games s |
| SeaDreams | shareware games s |
| Search Installer | Acrobat Reader 301 |
| SeaWolf Docs | shareware games s |
| SG Manual | shareware games s |
| ShabongClub0.9b(E) | shareware games s |
| Shadow of the Dragon part 1 | shareware games s |
| Shallow Grave | shareware games s |
| Shallow Grave 1.0 | shareware games s |
| ShareDraw Users Guide | shareware games s |
| Shatterball | shareware games s |
| Shatterball 0.29 | shareware games s |
| ShatterBat 1.0.0 Installer | shareware games s |
| ShatterBat 1.1 | shareware games s |
| ShatterBat 1.1 Installer | shareware games s |
| Shattered Steel Demo | shareware games s |
| Shattered Stone 1.0 | shareware games s |
| Shattered Stone Instructions | shareware games s |
| Shockwave Demo | shareware games s |
| Shootin the breeze... | shareware games s |
| ShootMe | shareware games s |
| Short Circuit | shareware games s |
| Short Circuit 1.2.3 | shareware games s |
| Short Circuit v1.01 | shareware games s |
| Short Circuit v1.2.2 | shareware games s |
| Showwit! Solver | shareware games s |
| Shufflepuck 1.5 | Shufflepuck1.5 |
| SideMinder! 1.1 | shareware games s |
| Siege | shareware games s |
| Siege of Darkwood 1.0 | shareware games s |
| Sifter 1.0 | shareware games s |
| Sifter User Guide | shareware games s |
| Sigma Chess 5.1.3 Shareware | shareware games s |
| Silicon Volleyball | shareware games s |
| Sim Cinema Deluxe 1.5 | shareware games s |
| Sim Cinema DX 2.0 Installer | shareware games s |
| Sim Cinema DX 2.1 Installer | shareware games s |
| Sim Tape Dispenser | shareware games s |
| sim tower cheat 35 | shareware games s |
| Sim-Cinema™ DELUXE Installer | shareware games s |
| SimBill '96 1.0 | shareware games s |
| SimBush1.2 | shareware games s |
| simMir 1.0 | shareware games s |
| simMir v1.0.0 | shareware games s |
| Simon Says 1.0 | shareware games s |
| SimPiglet1.1 | shareware games s |
| SimpleTetris 1.0 beta | shareware games s |
| SimpleText | Acrobat Reader 301 |
| SimpleText | Adobe Photoshop 5.toast  |
| SimpleText | HyperCard241 |
| SimpleText Enhancer 3.0 | shareware games s |
| Simpsons-Mortal Gunshy | shareware games s |
| SimStapler | shareware games s |
| SimStapler v.6.1 | shareware games s |
| SimTower Demo | shareware games s |
| SimTowerCHEAT(US) | shareware games s |
| Sindler's List  (68k) | shareware games s |
| SingleNumber (FAT) | shareware games s |
| SingleNumber (light) | shareware games s |
| SingleNumber(FAT) | shareware games s |
| Sireadh Seirce GÉ-0.8.7 | shareware games s |
| Sirtet | shareware games s |
| Sirtet Info | shareware games s |
| SISnikkDemo | Adobe Photoshop 5.toast  |
| Skat 1.1, 2.0 & 3.0 Order | shareware games s |
| Skat 1.1.2 | shareware games s |
| Skat for Beginners 1.1.2 | shareware games s |
| Skat for Beginners 2.02 | shareware games s |
| Skat Handbook 1.2 | shareware games s |
| Skat:DokoE (3.0:1.1) Demo | shareware games s |
| Skat:DokoE Order Form | shareware games s |
| SKELETON GOODIES | shareware games s |
| Ski3D 1.0 | shareware games s |
| Skittles | shareware games s |
| SkyDive 1.0 | shareware games s |
| Slabbers 1.0 | shareware games s |
| Sleeper Documentation | shareware games s |
| Slick Willie III 0.6 | shareware games s |
| Sliders FAT | shareware games s |
| Slime Invaders 2.0.1 | shareware games s |
| Slippy(E) | shareware games s |
| Slippy(fat)-E | shareware games s |
| Slithereens 1.0 | shareware games s |
| SM1.5->1.6.2 | shareware games s |
| Smack a Skunk 1.0 | shareware games s |
| Smack a Skunk 1.1 | shareware games s |
| SmallMines 1.0 | shareware games s |
| SmallMines 2.0 (FAT) | shareware games s |
| SmallWorm 1.4 | shareware games s |
| Smart Move 1.0.2 | shareware games s |
| Smashing Windows Docs | shareware games s |
| Smashing Windows II | shareware games s |
| Smashing Windows v5.5 | shareware games s |
| SMILE•The Splattering 1.2 DEMO | shareware games s |
| Smoothie | shareware games s |
| Smoothie User Guide | shareware games s |
| Smoothie Users Guide | shareware games s |
| Snack Attack | shareware games s |
| Snack Attack FAT v1.1.0 | shareware games s |
| Snail Race 1.0 | shareware games s |
| Snake Byte 1.1 | shareware games s |
| Snakey Math 1.0.1 | shareware games s |
| Snapz Pro 2.0.1 Trial | shareware games s |
| Sniffer Dog™ Installer | shareware games s |
| Snit 2 | shareware games s |
| Snit 2! | shareware games s |
| SnoodEdit | shareware games s |
| Snoop 1.0 Demo | shareware games s |
| Snow | shareware games s |
| Snow 2.0.sea | shareware games s |
| So Far | shareware games s |
| SOD Read Me | shareware games s |
| Software by RFB | shareware games s |
| Sogni & Spade Demo | shareware games s |
| Sokoban 1.1.1 | shareware games s |
| SokobanEditor | shareware games s |
| Soldiers Manual | shareware games s |
| Soldiers of the Sun 3.0 (68K) | shareware games s |
| SOLEX Startup | shareware games s |
| Solitaire | shareware games s |
| Solitaire House (fat) 1.0.1 | shareware games s |
| Solitaire House (fat) 1.0.2 | shareware games s |
| Solitaire House (fat) 1.1.1 | shareware games s |
| Solitaire Till Dawn Games Guide | shareware games s |
| Solitaire Till Dawn User Guide | shareware games s |
| Sound Match | shareware games s |
| Sound Studio 1.4 | shareware games s |
| SoundApp Fat | sound-app-241-fat |
| SoundEdit Pro Install | SoundEditPro105.sea  |
| SoundEdit Pro™ 1.0.5 | SoundEditPro105.sea  |
| SoundEdit™ | SoundEditPro105.sea  |
| SoundEdit™ | SoundEdit 2.0.5 |
| Sounds Like v1.0 | shareware games s |
| SP GunShy 1.3 | shareware games s |
| Space Debris Docs | shareware games s |
| Space Debris v1.0.1 | shareware games s |
| Space Invaders 1.1 | shareware games s |
| Space Joust 2.0 | shareware games s |
| Space Joust Manual | shareware games s |
| Space Madness™ Demo Manual | shareware games s |
| Space Pebbles 1.1 | shareware games s |
| Space Wumpus 1.0 | shareware games s |
| SpaceBattle 1.01 | shareware games s |
| SpaceDeubza 1.01 | shareware games s |
| SpaceDeubza 2.0 | shareware games s |
| SpaceInvader! 1.02 | shareware games s |
| SpaceInvader! 1.04 | shareware games s |
| Spacestation Pheta | Cool Games Two |
| Spacestation Pheta | shareware games s |
| SPAM v5.0 | shareware games s |
| Sparkz™ | shareware games s |
| Spectre 1.0 | spectre |
| Spectre World Editor | Spectre World Editor 2.1 |
| Spectre World Editor 2.1 | spectre |
| Sphericon X Manual | shareware games s |
| Sphericon X v1.0 | shareware games s |
| Spherules 2.0 | shareware games s |
| Spider And Web | shareware games s |
| Spiked 2.0 | shareware games s |
| Spiked 2.1 | shareware games s |
| Spins Demo | shareware games s |
| SpiralGraphics | shareware games s |
| SpookHouse | shareware games s |
| Sprite Fight 2002 | shareware games s |
| Sprite Fight Movie Player | shareware games s |
| Sprockets R Us | Tempest2000 |
| Squad-Hex | shareware games s |
| Squad-Hex Editor | shareware games s |
| Squark 1.3a | shareware games s |
| Squark Game Installer | shareware games s |
| Squirt Derby! Manual | shareware games s |
| Squish 2.6.5 | shareware games s |
| Squish 2.6.6 | shareware games s |
| Squish DEMO | shareware games s |
| Squish v2.6.1 | shareware games s |
| Squish v2.6.2 | shareware games s |
| SS Warlock Movie Trailer | spaceship-warlock-1996 |
| SS Warlock Movie Trailer AV | spaceship-warlock-1996 |
| SS-RoadKill | shareware games s |
| Stacked Roller! | shareware games s |
| Star Chaos Docs | shareware games s |
| Star Chaos v1.0 | shareware games s |
| Star Chaos v1.0.1 | shareware games s |
| Star Chaos v1.2 (FAT) | shareware games s |
| Star Chaos v1.3 FAT | shareware games s |
| Star Fighters 1.01 | shareware games s |
| Star Fighters 1.02 | shareware games s |
| Star Fighters 1.3 | shareware games s |
| Star Fighters 1.4 | shareware games s |
| Star Fighters 1.4.1 | shareware games s |
| Star Fighters 1.6 | shareware games s |
| Star Fleet Command 1.0 | shareware games s |
| Star Patrol | shareware games s |
| Star Patrol 2.0 | shareware games s |
| Star Seed 1.1 | shareware games s |
| Star Seed v1.1 | shareware games s |
| Star Stormer 0.24 | shareware games s |
| StarBound | shareware games s |
| Starbound II 68k | shareware games s |
| Stardust | shareware games s |
| Stardust 1.1 | shareware games s |
| STARFIGHT-FINAL(E) | shareware games s |
| StarGates 1.0 | shareware games s |
| StarGates 1.0 installer | shareware games s |
| StarGuardian | shareware games s |
| Starry Night 1.0.3 Updater | shareware games s |
| Starry Night Installer | shareware games s |
| Starship v1.0.1 | shareware games s |
| Step On It! Demo Installer | shareware games s |
| Stocks! | shareware games s |
| Stones  II | shareware games s |
| Stones II Documentaion | shareware games s |
| StoryMaker 1.0 | shareware games s |
| Strategic Overrun 1.0 | shareware games s |
| Strategram™ Installer | shareware games s |
| Street Rumble | shareware games s |
| STRiKE Korea '97 Docs | shareware games s |
| STRiKE Korea '97 Installer | shareware games s |
| String Up Steve 2.0 | shareware games s |
| Strip-Mac! | shareware games s |
| StuffIt Expander™ | stuffit expander 55 |
| StuntCopter1.2 | StuntCopter1.2 |
| Super Connecto 1.5.5 | shareware games s |
| Super Mac Freecell 1.0 | shareware games s |
| Super Mac Freecell 1.1 | shareware games s |
| Super Mac Freecell 1.1.1 | shareware games s |
| Super Mac Freecell 1.1.2 | shareware games s |
| Super Mac Freecell 1.5 | shareware games s |
| Super Othello 2.2.1 (FAT) | shareware games s |
| Super Othello 2.3.1 (FAT) | shareware games s |
| Super Othello 2.3.2 (FAT) | shareware games s |
| Super Spectre Editor | spectre |
| SuperBike | shareware games s |
| SuperMacMan 1.0d6 | shareware games s |
| SuperMemory 3.1.1 | shareware games s |
| SuperPaint 2.0 | SuperPaint2.0 |
| SuperSpam! 1.2 | shareware games s |
| SUSAN™ 1.5 | Cool Games One |
| SUSAN™ 2.5 | shareware games s |
| Swap 'em Over 1.5.1 | shareware games s |
| Swoop 1.0.1 Installer | shareware games s |
| Swoop 1.0.2 | shareware games s |
| Swoop Installer | shareware games s |
| Sword | shareware games s |
| Sword Dream | shareware games s |
| Sword Dream (FAT) | shareware games s |
| Sword Dream 3D | shareware games s |
| Sword of Ages 2.1 | shareware games s |
| System Errors 7.0.1 | SystemErrors |
| System's Twilight | shareware games s |
| System's Twilight v1.0.5 | shareware games s |
| TeachText | Adobe-1-0 |
| TeachText | Adobe Photoshop 5.toast  |
| TeachText | Excel 5.0 English |
| TeachText | FREAKSHOW 1994 |
| TeachText | WordPerfect-3-install-set |
| Technical Manual | shareware games s |
| Tempest 2000™ 1.0.2 Updater | Tempest2000Updater |
| Tempest 2000™ Installer | Tempest2000 |
| Tetris Max 2.8 | TetrisMax 2.8 |
| Texthanteraren | Microsoft Word 5.0 sv |
| The Light - Shelby's Addendum | shareware games s |
| The Print Shop | PrintShop132 |
| The Sound-Trecker V2.2 | sound-trecker 2.2 |
| Total Distortion DEMO INFO! | spaceship-warlock-1996 |
| Trek 1.4.2 | Cool Games One |
| Version History | shareware games s |
| Welcome | welcome 1.2.1.sea  |
| •••Special Offer Catalog•• | shareware games s |
| ∑ Chess 4.0 Lite | shareware games s |

## Ran and quit (144)

| Application | From | 
|---|---|
| Add Alias to Apple Menu | HyperCard241 |
| Adobe Photoshop™ 1.0 | Adobe-1-0 |
| Alert When Folder Changes | HyperCard241 |
| American Heritage Dictionary | American Heritage Del 3 |
| Apple Guide Plug-In | shareware games s |
| Auto batch Line art | Adobe Photoshop 5.toast  |
| BackupFolderToURL | fetch |
| BadDay (Non-Power PC) | BadDay.toast  |
| BadDay (Power PC) | BadDay.toast  |
| BadDay (Universal) | BadDay.toast  |
| Blackjack | shareware games s |
| BoxTop Registration Client | shareware games s |
| Change Monitor to 256 | HyperCard241 |
| Change Monitor to B&W | HyperCard241 |
| Client Installer | American Heritage Del 3 |
| Color to printer | Adobe Photoshop 5.toast  |
| Commotion 1.5 DEMO | Adobe Photoshop 5.toast  |
| Commotion™1.5Demo Installer | Adobe Photoshop 5.toast  |
| Diamonds | Diamonds 1.5 |
| Double Click to Read | Cool Games Two |
| Double Click to Read | shareware games s |
| Double sided Line art | Adobe Photoshop 5.toast  |
| DropUpload | fetch |
| Echo | shareware games s |
| Find Original from Alias | HyperCard241 |
| FreakShow Projector | FREAKSHOW 1994 |
| Glypha II 1.1 | Cool Games One |
| Grayscale to (fax) | Adobe Photoshop 5.toast  |
| Grayscale to printer | Adobe Photoshop 5.toast  |
| Guess the number! | shareware games s |
| Hangman | shareware games s |
| Hide:Show Folder Sizes | HyperCard241 |
| High Score Saver | TetrisMax 2.8 |
| iNCOMING | BadDay.toast  |
| Install Adobe Photoshop® 5.0 | Adobe Photoshop 5.toast  |
| Installeraren 3.3 | Microsoft Word 5.0 sv |
| Line art to (fax) | Adobe Photoshop 5.toast  |
| Line art to printer | Adobe Photoshop 5.toast  |
| MacAMP 1.0b7 | MacAMP1.0b7 |
| MacAMP Site | MacAMP1.0b7 |
| MacSokoban score mover | Cool Games One |
| MacSokoban score mover | shareware games s |
| Matcher | shareware games s |
| Mille Bornes | Cool Games Two |
| Out of this World 1.0 | out of this world |
| Out of this World™ (1.0.2) | out of this world |
| Out of this World™ 1.0 -> 1.0.2 | out of this world |
| Out of this World™ 1.0.3 | out of this world |
| Pict -> Tile | shareware games s |
| Pict -> Tíle | shareware games s |
| PopChar Pro Installer | shareware games s |
| Rock, Paper, AppleScript™ | shareware games s |
| Room Editor 1.04 | Glider 42 |
| S.C. Out | shareware games s |
| S.C.OUT | scout 1 4 |
| Safari 1.0 | shareware games s |
| Sapiens 2.2 | shareware games s |
| Sapiens Installation | shareware games s |
| Scrambler | shareware games s |
| Screen Test 1.0 | shareware games s |
| ScreenTest | shareware games s |
| Script Editor | HyperCard241 |
| Setback Deluxe 1.0fc7 | shareware games s |
| Severed Ed 1.1 web | shareware games s |
| Share a Folder | HyperCard241 |
| ShareDraw | shareware games s |
| Showwit! | shareware games s |
| SimonSaysDeluxe game | shareware games s |
| Sleep Now | shareware games s |
| Slider | shareware games s |
| Snavely! | shareware games s |
| Snavely! 1.0.4 | shareware games s |
| Snavely! 1.0.5 | shareware games s |
| Snood | shareware games s |
| Soi | shareware games s |
| soi 1.3.2 | shareware games s |
| soi 1.4 | shareware games s |
| Soi 1.4 Freeware | shareware games s |
| soi 1.7 | shareware games s |
| Sokoban3D 1.0 FAT | shareware games s |
| Sokoban3D 1.0.1 | shareware games s |
| Solarian II | Cool Games One |
| Solarian II | shareware games s |
| SoundJam MP Free | shareware games s |
| SoundJam MP Installer | shareware games s |
| Space Cab | shareware games s |
| Space Cab 1.2 Installer | shareware games s |
| Space Cab Installer | shareware games s |
| Space Gypsy 1.0 | shareware games s |
| Space Gypsy 1.0.1 | shareware games s |
| Space Gypsy Installer | shareware games s |
| Space Gypsy.sea | shareware games s |
| Space Junkie | shareware games s |
| Space Junkie 1.1 | shareware games s |
| Space Junkie 1.2 | shareware games s |
| Space Junkie 1.2a | shareware games s |
| Space Junkie 1.2a | space-junkie-12a |
| Spaceship Warlock™ AV | spaceship-warlock-1996 |
| Spades Deluxe | shareware games s |
| Spades Deluxe 2.0 | shareware games s |
| Spades Deluxe 2.0 fc7 | shareware games s |
| Spectre | spectre |
| Spectre 1.0#1 | spectre |
| Spectre 1.0#2 | spectre |
| Spectre 1.0ß2 | spectre |
| Spectre 1.1 | Spectre 1.1 |
| Spectre Challenger | spectre |
| Spheres | shareware games s |
| Sprite Fight 2002 Masker | shareware games s |
| SpriteFight2002-Flip'nMask | shareware games s |
| Squark Game | shareware games s |
| SS II | shareware games s |
| Star Chaos | shareware games s |
| Star Chaos v1.4 (FAT) | shareware games s |
| Star Flick | shareware games s |
| Star Trek® Demo | shareware games s |
| StarCraft 1.05 Demo | shareware games s |
| Starcraft Demo | shareware games s |
| StarRunner Plus | shareware games s |
| Starry Night PPC Installer | shareware games s |
| Starry Night v1.0.1 | shareware games s |
| Starry Night v1.0.1 (PowerMac) | shareware games s |
| Start File Sharing | HyperCard241 |
| StartupFrills | shareware games s |
| Stop File Sharing | HyperCard241 |
| Super Munchers | shareware games s |
| SuperColumns | shareware games s |
| SuperGIF 1.0 | shareware games s |
| SuperKeno | shareware games s |
| Supreme Commander | shareware games s |
| Suspend LiveCard Service | HyperCard241 |
| Synchronize Folders | HyperCard241 |
| Talking Spelling Bee 2.5 | shareware games s |
| TD Demo 12 Meg | spaceship-warlock-1996 |
| TD Demo 4 Meg | spaceship-warlock-1996 |
| TD Demo 6 Meg | spaceship-warlock-1996 |
| TD Demo 8 Meg | spaceship-warlock-1996 |
| The Sentence Builder v1.0 | shareware games s |
| Tic-Tac-Toe | shareware games s |
| Truly Supreme | spectre supreme |
| Turn Sound Off | HyperCard241 |
| Turn Sound On | HyperCard241 |
| War | shareware games s |
| Zero 2-20 | Glider 42 |

## Running at the limit (54)

| Application | From | 
|---|---|
| 'PDF ' | Acrobat Reader 301 |
| Compact Pro | CompactPro1.50 |
| Director 4.0 Installer | macromedia director 4 |
| DiskDup+ AutoTyper | DiskDup2.9.2 |
| Double-Click Me To Install | after dark 3.0 |
| Engine 3D Demo 68040 | shareware games s |
| Face-Up Solitaire | shareware games s |
| Install Bitstream Fonts | WordPerfect-3-install-set |
| Install Language Module | WordPerfect-3-install-set |
| Install WordPerfect | WordPerfect-3-install-set |
| Journeyman Demo | Journeyman.toast  |
| Journeyman Looping Demo | Journeyman.toast  |
| MacPlaymate™ | MacPlaymate |
| Macromedia Director 5 Install | Macromedia Director 5.0 |
| MacWrite 1.6 | macwrite 1.6 |
| Prince of Persia | PoP Collection Limited.cdr  |
| Prince of Persia | Prince Of Persia |
| ResEdit | ResEdit 2.1.3 |
| ResEdit | resedit 2.1.3.sea  0 |
| Secret Agent Joe 1.1 | shareware games s |
| Shards | shareware games s |
| Shards 1.3 | shareware games s |
| Shooting Star | shareware games s |
| Shufflepuck Café (cheats) | SPuck |
| Shufflepuck Café (nude hack) | SPuck |
| Sir Richard... | shareware games s |
| Sirtet Voyager FAT | shareware games s |
| SNES9X ChoiceStick settings | shareware games s |
| Sokoban 2.0 | shareware games s |
| Sokoban 2.1 | shareware games s |
| Sokoban 2.2 | shareware games s |
| Sokoban 2.3 | shareware games s |
| Sokoban 2.4 | shareware games s |
| Sokoban 2.4.2 | shareware games s |
| Sokoban 2.4.2a | shareware games s |
| Solitaire Till Dawn™ | shareware games s |
| Sound Sculptor II | shareware games s |
| Space Debris | shareware games s |
| Space Debris (FAT) | shareware games s |
| Space Debris v1.3 (FAT) | shareware games s |
| Space Debris v1.4 | shareware games s |
| Space Debris v1.4 (FAT) | shareware games s |
| SpaceCamel | shareware games s |
| SpaceFight B&W v1.1 | shareware games s |
| Spaceship Warlock™ | spaceship-warlock-1996 |
| Spectre Supreme Installer | spectre supreme |
| Spectre VR Installer | SpectreVR1.0.5 |
| Speed Demon Settings | shareware games s |
| Splash! FAT | shareware games s |
| Squirt Derby! | shareware games s |
| StarGazer's Delight 4.5.1 E | shareware games s |
| Strategram™ Demo Installer | shareware games s |
| SUMO 1.0.2 | shareware games s |
| SUMO™ | shareware games s |

## Stopped: other (8)

| Application | From | Where it stopped |
|---|---|---|
| Sail Away | shareware games s | TRAPV (vector 7) at 004020F4, sr 2002, op 0640 |
| ShatterBat | shareware games s | alert 996: A fatal internal error has occurred: |
| Shrink | shareware games s | alert 129: An error of type 1001 occurred. |
| Shufflepuck Café v1.0 | SPuck | illegal instruction (vector 4) at 004011DA, sr 2004, op 4C84 |
| Space Joust 1.0.2 | shareware games s | exception 10 at pc 00401BD6 with no handler |
| Space Madness™ Demo | shareware games s | illegal instruction (vector 4) at 00431618, sr 2008, op 7F7F |
| Starry Night Backyard 3.03 | shareware games s | LoadSeg: cannot load CODE 64 |
| Sunflower Puzzle | shareware games s | F-line (no FPU) (vector 11) at F8000017, sr 2008, op FF00 |

## Too slow (45 s timeout) (83)

| Application | From | 
|---|---|
| Action, Jackson.sea | Cool Games One |
| AppleWorks Updater 5.0.4.smi | AppleWorks5.0.4 |
| Bad Day Installer | BadDay.toast  |
| BBEdit Lite 6.1.2.smi | bbedit lite 612.smi  |
| Brain Salad Surgery.sea | Cool Games Two |
| Chromatica™ Installer | Adobe Photoshop 5.toast  |
| ClipMedia™ sound install | SoundEditPro105.sea  |
| Cool PowerBook Games.sea | Cool Games One |
| EdgeWizard™ Demo Installer | Adobe Photoshop 5.toast  |
| Extra Chewing Satisfaction.sea | Cool Games Two |
| Improved Art Files.sea | Glider 42 |
| Install Document Experts™ | WordPerfect-3-install-set |
| Install Eye Candy 3.0.1 Demo | Adobe Photoshop 5.toast  |
| Internet Explorer 5.1.7.smi | Internet Explorer 5.1.7.smi  |
| LaserSeps™Pro Demo Installer | Adobe Photoshop 5.toast  |
| Mac Parlor Games.sea | Cool Games Two |
| MagicMask™ Demo Installer | Adobe Photoshop 5.toast  |
| Page Edges 3.0 Demo | Adobe Photoshop 5.toast  |
| PhotoCell™ Demo Installer | Adobe Photoshop 5.toast  |
| PhotoGraphic Patterns Demo | Adobe Photoshop 5.toast  |
| PhotoSpot™ct Demo Installer | Adobe Photoshop 5.toast  |
| Save the World 1.02 | shareware games s |
| ScanTastic® Apple Demo | Adobe Photoshop 5.toast  |
| ScanTastic® Epson Demo | Adobe Photoshop 5.toast  |
| ScanTastic® HP Demo | Adobe Photoshop 5.toast  |
| ScrapIt Pro 5.52 | shareware games s |
| Scroll Bars 1.0 | shareware games s |
| SCS Connect Four 1.0.1 | shareware games s |
| SCS Mines | shareware games s |
| SCSI Probe 5.2 | shareware games s |
| Search Designer | shareware games s |
| Sentinels of Ceth 1.5 | shareware games s |
| Serotonin 1.5 | shareware games s |
| Shadow Warrior Demo.sea | shareware games s |
| SideMinder | shareware games s |
| Sifter Installer | shareware games s |
| Sigma Chess Lite 4.0 | shareware games s |
| Sim Cinema Deluxe 2.1 | shareware games s |
| Sindler's List 1.0.4 Installer | shareware games s |
| Sirtet Voyager | shareware games s |
| sixtyforce 0.5.5 | shareware games s |
| Sleeper 2.0.1.sea | shareware games s |
| smartGIF™ 1.0.1 Installer | Adobe Photoshop 5.toast  |
| Smashing Windows II 1.0 | shareware games s |
| Smoothie 1.1.1 Installer | shareware games s |
| Snack Attack 1.0 | shareware games s |
| Snavely 1.0.4 | shareware games s |
| Snavely 1.0.5 | shareware games s |
| SNES9X 1.1.1.1 | shareware games s |
| Snood 2.0 | shareware games s |
| Solitaire House 1.0.1 | shareware games s |
| SolitaireTillDawn | shareware games s |
| SoundEditPro105 | SoundEditPro105.sea  |
| SoundMaster Installer | shareware games s |
| Space Bug 1.1 | shareware games s |
| SpaceMadness™Demo Installer | shareware games s |
| Spades Deluxe 1.5 | shareware games s |
| Speed Demon 1.2 | shareware games s |
| SpiralGraphics 2.0 | shareware games s |
| Spot Box QuickTime Games Free | shareware games s |
| Sprint | shareware games s |
| Sproing 1.0 | shareware games s |
| Squirrel Kombat 1.0 | shareware games s |
| Squirt Derby | shareware games s |
| Star Chaos 2.0.1 | shareware games s |
| Star Wars DroidWorks Demo 1.0 | shareware games s |
| Starbound 2 | shareware games s |
| Starbound II .91 | shareware games s |
| Starbound II .98 | shareware games s |
| Starbound II .98S | shareware games s |
| StarFight-Final 1.1 | shareware games s |
| StarGates 1.0 installer.sea | shareware games s |
| StarGazer's Delight 4.5.1 e | shareware games s |
| StarLands 1.0 | shareware games s |
| States 3000 1.0.1 | shareware games s |
| Stikes Revenge 2.sea | shareware games s |
| Street Rumble 1.1 | shareware games s |
| Stupid AppleScript Games 1.2 | shareware games s |
| Super Snake shareware | shareware games s |
| Surgery! 3.01 | ?  shareware games s |
| Ultimate Texture Demo Installer | Adobe Photoshop 5.toast  |
| Universal Animator Demo | Adobe Photoshop 5.toast  |
| Victoire.sea | Cool Games Two |
