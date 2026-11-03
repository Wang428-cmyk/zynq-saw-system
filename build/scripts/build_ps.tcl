# Vitis 2021.2 XSCT entry. Use tools/build_ps.py for path-safe staging/logging.
# Argument: a fresh ASCII stage containing inputs/, source/, scripts/.
proc main {} {
    global argv
    if {[llength $argv] != 1} {error "Usage: xsct build_ps.tcl <stage>"}
    set stage [file normalize [lindex $argv 0]]
    set ws [file join $stage workspace]
    set xsa [file join $stage inputs top_module.xsa]
    set src [file join $stage source]
    set out [file join $stage artifacts]
    if {[file exists $ws]} {error "Workspace must be new: $ws"}
    file mkdir $out
    puts "SAW_TOOL_VERSION: [version]"
    if {![string match "*2021.2*" [version]]} {error "This build profile requires Vitis 2021.2."}
    setws $ws
    platform create -name saw_platform -hw $xsa
    platform active saw_platform
    domain create -name saw_domain -os standalone -proc ps7_cortexa9_0
    domain active saw_domain
    source [file join $stage scripts bsp_config.tcl]
    platform write
    platform generate
    puts "SAW_PHASE: application"
    app create -name saw_app -platform saw_platform -domain saw_domain -template {Empty Application(C)}
    importsources -name saw_app -path $src -linker-script
    app config -name saw_app build-config Debug
    app config -name saw_app linker-script [file join $ws saw_app src lscript.ld]
    app config -name saw_app -add include-path [file join $ws saw_app src]
    app config -name saw_app -add include-path [file join $ws saw_app src ad9361]
    app config -name saw_app -add libraries lwip4
    app config -name saw_app -add libraries xilffs
    app build -name saw_app
    set elf [file join $ws saw_app Debug saw_app.elf]
    if {![file exists $elf]} {error "Application ELF missing: $elf"}
    file copy -force $elf [file join $out saw_app.elf]
    # Default boot components use a separate FSBL BSP, not the lwIP application BSP.
    set fsbl [file join $ws saw_platform export saw_platform sw saw_platform boot fsbl.elf]
    if {![file exists $fsbl]} {
        error "Generated FSBL missing at expected Vitis 2021.2 export path: $fsbl"
    }
    file copy -force $fsbl [file join $out fsbl.elf]
    file copy -force $xsa [file join $out top_module.xsa]
    file copy -force [file join $stage inputs top_module.bit] [file join $out top_module.bit]
    set app_mss [file join $ws saw_platform ps7_cortexa9_0 saw_domain bsp system.mss]
    if {[file exists $app_mss]} {file copy -force $app_mss [file join $out application_bsp.mss]}
    set bif [file join $out boot_image.bif]
    set handle [open $bif w]
    puts $handle "the_ROM_image:"
    puts $handle "\{"
    puts $handle "  \[bootloader\] \"[file join $out fsbl.elf]\""
    puts $handle "  \"[file join $out top_module.bit]\""
    puts $handle "  \"[file join $out saw_app.elf]\""
    puts $handle "\}"
    close $handle
    puts "SAW_PHASE: bootgen"
    puts [exec bootgen -arch zynq -image $bif -o [file join $out BOOT.BIN] -w on 2>@1]
    if {![file exists [file join $out BOOT.BIN]]} {error "BOOT.BIN missing"}
    puts "SAW_BUILD_SUCCESS"
}
if {[catch {main} failure options]} {
    puts stderr "SAW_BUILD_FAILED: $failure"
    if {[dict exists $options -errorinfo]} {puts stderr [dict get $options -errorinfo]}
    exit 1
}
exit 0

