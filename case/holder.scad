// ESP32 Flight Tracker holder.
// A plain tube: stands on its own end face, bends 45 deg, and the board drops
// into the end face at the top. No screws.
//
// One bore, no steps. The glass is 75 mm and the board 71 mm, so the board
// slides in until the glass overhang lands on the tube's top rim - the rim is
// the stop, and the bore grips the PCB edge. Face points up at 45 deg, so
// gravity holds it there.
//
// Render:   openscad -o holder.stl holder.scad
// Fit test: part = "ring" first - 10 minutes, and it tells you whether the
//           press fit is right before you spend hours on the case.

part = "case";          // "case" | "ring"

/* ---- MEASURED ------------------------------------------------------- */
pcb_d    = 71.0;        // widest point of the board - the snug fit
glass_d  = 75.0;        // widest point of the glass - rests on the rim

/* ---- FIT ------------------------------------------------------------ */
clear    = 0.25;        // press fit on the PCB, PLA through a 0.4 nozzle

/* ---- SHAPE ---------------------------------------------------------- */
wall     = 2.4;         // 6 perimeters at 0.4. Also sets the rim the glass
                        // sits on: wall - (glass_d - pcb_d)/2 - clear
tilt     = 45;          // how far the tube bends
foot_h   = 25;          // straight bit standing on the desk
bend_r   = 55;          // centreline radius of the bend; bigger = lazier curve
usb_w    = 12.0;        // notch width - the plug body, not just the connector
notch_d  = 10.0;        // how deep the notch cuts in from the face
usb_a    = 45;          // both notches sit this far round the face from its
                        // lowest point, one either side

$fn = 160;

bore_d  = pcb_d + 2*clear;
outer_d = bore_d + 2*wall;
R       = outer_d/2;
face_x  = bend_r*cos(tilt);
face_z  = foot_h + bend_r*sin(tilt);

module tube_section() {
    difference() {
        circle(d = outer_d);
        circle(d = bore_d);
    }
}

// Work on the tilted end face: +z out of it, -x down the slope.
module on_face() {
    translate([face_x, 0, face_z])
        rotate([0, -tilt, 0])
            children();
}

module body() {
    // Foot, standing on the desk
    translate([bend_r, 0, 0])
        linear_extrude(foot_h + 1)
            tube_section();

    // The bend. A rotate_extrude end cap is already square to the tube, so
    // the tilted face needs no cutting - it is just where the sweep stops.
    translate([0, 0, foot_h])
        rotate([90, 0, 0])
            rotate_extrude(angle = tilt)
                translate([bend_r, 0])
                    tube_section();
}

// Two notches in the rim, cutting down from z = 0, for the connectors that
// stand proud of the board edge. Shared with the fit-test ring so one small
// print checks the bore and the port positions together.
module notch_cuts() {
    for (a = [usb_a, -usb_a])
        rotate([0, 0, 180 + a])
            translate([R, 0, -notch_d])     // bottom of the notch
                linear_extrude(notch_d + 5) // up and out through the face
                    square([2*wall + 8, usb_w], center = true);
}

module case() {
    difference() {
        body();
        on_face() notch_cuts();
        // Keep the foot's end face flat on the desk
        translate([0, 0, -100]) cube([600, 600, 200], center = true);
    }
}

// The fit test: the real bore and the real notches. Taller than the notch is
// deep, or the cuts sever the wall and it prints as two loose arcs.
module ring() {
    ring_h = notch_d + 6;
    difference() {
        cylinder(d = outer_d, h = ring_h);
        translate([0, 0, -1]) cylinder(d = bore_d, h = ring_h + 2);
        translate([0, 0, ring_h]) notch_cuts();
    }
}

if (part == "ring") ring(); else case();
