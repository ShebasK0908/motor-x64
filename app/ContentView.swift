import SwiftUI

struct Resultado: Identifiable {
    let id = UUID()
    let nombre: String
    let esperado: String
    let obtenido: String
    let pasos: String
    let ok: Bool
}

struct ContentView: View {
    @State private var resultados: [Resultado] = []
    @State private var totalPasos: String = "0"
    @State private var ejecutado = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Un motor de CPU x86-64 funcionando dentro de tu iPhone.")
                            .font(.headline)
                        Text("Ejecuta código máquina real de PC/PS5, instrucción a instrucción, sin JIT y sin trucos. Estas pruebas son programas x86-64 de verdad corriendo ahora mismo en este teléfono.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 4)
                }

                Section("Pruebas del motor") {
                    if !ejecutado {
                        Text("Ejecutando…")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(resultados) { r in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(r.nombre).font(.body)
                                Text("Esperado \(r.esperado) · Obtenido \(r.obtenido) · \(r.pasos) instrucciones")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: r.ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                                .foregroundStyle(r.ok ? .green : .red)
                        }
                    }
                }

                Section {
                    Text("Total: \(totalPasos) instrucciones x86-64 ejecutadas en esta sesión.")
                        .font(.subheadline)
                }

                Section("Qué es y qué no es") {
                    Text("Esto es la primera piedra de un emulador: el motor que entiende el idioma de los juegos de PS5 (x86-64). Funciona, pero va a velocidad de intérprete.")
                        .font(.subheadline)
                    Text("Todavía NO juega a PS5: faltan la capa del sistema de la consola y la parte gráfica. Ningún programa en el mundo las tiene hoy para iPhone.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("MotorX64")
            .onAppear { ejecutar() }
        }
    }

    private func ejecutar() {
        guard !ejecutado else { return }
        ejecutado = true
        DispatchQueue.global(qos: .userInitiated).async {
            let informe = String(cString: motor_report())
            var filas: [Resultado] = []
            var total = "0"
            for linea in informe.split(separator: "\n") {
                let p = linea.split(separator: "|", omittingEmptySubsequences: false)
                if p.count == 7 {
                    filas.append(Resultado(
                        nombre: String(p[0]),
                        esperado: String(p[1]),
                        obtenido: String(p[2]),
                        pasos: String(p[5]),
                        ok: p[6] == "OK"))
                } else if p.count == 2 && p[0] == "TOTAL" {
                    total = String(p[1])
                }
            }
            DispatchQueue.main.async {
                resultados = filas
                totalPasos = total
            }
        }
    }
}
