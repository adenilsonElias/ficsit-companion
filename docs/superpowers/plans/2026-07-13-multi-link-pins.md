# Pinos multi-link no Production Planner — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** No Production Planner, um pino aceita N links; a taxa do pino é a soma das taxas dos seus links, e ligar um consumidor novo a um pino já ligado faz a produção subir para atender a soma da demanda.

**Architecture:** `Pin::link` (ponteiro único) vira `Pin::links` (vetor), e `Link` ganha taxa própria. No `RateSolver`, a equação de igualdade por link (`taxa(saída) − taxa(entrada) = 0`) é substituída por uma variável por link mais uma equação de balanço por pino (`soma(links do pino) − taxa(pino) = 0`). Para um pino de um link só, as duas formulações são idênticas — por isso todos os testes existentes devem passar sem alteração.

**Tech Stack:** C++17, CMake, Catch2, Dear ImGui + ImGui Node Editor, `FractionalNumber` (aritmética racional exata).

## Global Constraints

- **Todo texto que vai para o código é em inglês.** Comentários, nomes de `TEST_CASE`, blocos `/// @test` e `/// @covers`, identificadores, mensagens de erro e strings de log: **inglês**, sem exceção. É a língua que o repositório já usa em 100% do código. Este plano e o spec estão em português porque são documentos de trabalho; **nada de português atravessa a fronteira para dentro de um `.cpp` ou `.hpp`.** Os blocos de código deste plano já estão em inglês e devem ser copiados como estão.
- **Sem operações de git.** Não commite, não crie branch, não crie worktree. Ao fim de cada tarefa, registre o progresso em `PROGRESS.md` (na raiz do repo). Esta é uma preferência explícita do usuário e substitui o passo "Commit" padrão.
- **Escopo é o Production Planner.** `FactorySnapshotApp`, `VehicleMapApp` e `sav_import` continuam com no máximo um link por pino. Eles não criam links interativamente, então basta que continuem compilando e passando nos testes — nenhuma feature nova lá.
- **Aritmética exata.** Toda taxa é `FractionalNumber`. Nunca use `float`/`double` para taxa.
- **Compatibilidade de saves.** Um `.fcs` salvo antes desta mudança deve abrir e produzir um grafo idêntico.
- **A rede de segurança:** `test_graph_model`, `test_rate_propagation`, `test_rate_solver`, `test_rate_solver_phases`, `test_node_serialization`, `test_session_serializer`, `test_group_node`, `test_pin` e `test_sav_import` devem passar **sem edição** ao fim de cada tarefa, salvo onde o plano mandar editar explicitamente.

## Comandos

Build + testes (raiz do repo):

```bash
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure
```

Rodar um caso só:

```bash
./build/ficsit-companion/Release/fc-tests.exe "nome do TEST_CASE"
```

Filtrar por tag:

```bash
./build/ficsit-companion/Release/fc-tests.exe "[graph_model]"
```

## Estrutura de arquivos

| Arquivo | Responsabilidade | Tarefa |
|---|---|---|
| `include/domain/graph/pin.hpp` | `Pin::links` (vetor) + helper `SoleLink()` | 1 |
| `include/domain/graph/link.hpp` | `Link::current_rate` | 1 |
| `src/domain/graph/pin.cpp` | construtor; `SetLocked` propaga pelo link | 1, 3 |
| `src/domain/graph/graph_model.cpp` | `CreateLink` / `DeleteLink` / `DeleteNode` | 1, 3, 4 |
| `src/domain/graph/graph_item_resolve.cpp` | travessia de itens pelo grafo | 1 |
| `src/domain/nodes/organizer_nodes.cpp` | `RemoveItemIfNotForced` | 1 |
| `src/domain/nodes/group_node.cpp` | (de)serialização do subgrafo | 1, 5, 7 |
| `src/domain/snapshot/*.cpp` | snapshot: ≤1 link, migração mecânica | 1 |
| `src/infra/saveimport/sav_import.cpp` | importador: ≤1 link, migração mecânica | 1 |
| `src/domain/solver/rate_solver_internal.hpp` | `VariableMapping` ganha variáveis de link | 2 |
| `src/domain/solver/rate_solver_seed.cpp` | semear por *todos* os links de um pino | 2 |
| `src/domain/solver/rate_solver_variables.cpp` | uma variável por link relevante | 2 |
| `src/domain/solver/rate_solver_system.cpp` | equação de balanço por pino | 2 |
| `src/domain/solver/rate_solver_apply.cpp` | variável de link livre; gravar taxa no link | 2, 2b |
| `src/infra/persistence/session_serializer.cpp` | campo `rate` por link | 5 |
| `src/app/production_app.cpp` | `DragLink` (libera), `RenderLinks` (cor + tooltip) | 6 |

---

### Task 1: O modelo — `Pin::links` e `Link::current_rate`

Troca mecânica, **sem mudança de comportamento**. Ao fim desta tarefa nada de novo é possível na UI (o `DragLink` ainda rejeita pino ocupado); o que muda é só a forma de armazenar.

**Files:**
- Modify: `ficsit-companion/include/domain/graph/pin.hpp`
- Modify: `ficsit-companion/include/domain/graph/link.hpp`
- Modify: `ficsit-companion/src/domain/graph/pin.cpp:8` (construtor), `:26-33` (`SetLocked`)
- Modify: `ficsit-companion/src/domain/graph/graph_model.cpp:108-109`, `:279-311`, `:322-335`
- Modify: `ficsit-companion/src/domain/graph/graph_item_resolve.cpp:72-73`, `:107-114`, `:158-159`, `:178-179`
- Modify: `ficsit-companion/src/domain/nodes/organizer_nodes.cpp:109-123`
- Modify: `ficsit-companion/src/domain/nodes/group_node.cpp:79-80`
- Modify: `ficsit-companion/src/domain/snapshot/snapshot_bypass.cpp:25-26`
- Modify: `ficsit-companion/src/domain/snapshot/rate_propagation.cpp:59`, `:66`, `:87`, `:94`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_seed.cpp:28-29`, `:44-51`, `:76-84`, `:110-117`, `:126-134`, `:155-162`, `:205-213`, `:222`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_system.cpp:36-48`
- Modify: `ficsit-companion/src/infra/saveimport/sav_import.cpp` (linhas 382-384, 396, 405, 421, 1286, 1297, 1342, 1349-1350, 1373-1376, 1453-1457, 1490-1492, 1512-1513, 1547, 1560, 1573, 1588, 1592, 1767-1768, 1786-1787, 1794, 1801, 1811-1812, 1821, 1828)
- Modify: `ficsit-companion/src/app/production_app.cpp:358-364`, `:552-555`, `:1716-1726`, `:1910`, `:2057-2059`, `:2245`, `:2314-2316`, `:2829`, `:2861`
- Modify: `ficsit-companion/tests/graph_test_helpers.hpp:41-47` — **obrigatório**: `MakeLink` escreve `out_pin->link` / `in_pin->link` direto. Sem migrar este helper, nenhum teste compila. Vira:
  ```cpp
  inline std::unique_ptr<Link> MakeLink(unsigned long long id, Pin* out_pin, Pin* in_pin)
  {
      auto link = std::make_unique<Link>(ax::NodeEditor::LinkId(id), out_pin, in_pin);
      out_pin->links.push_back(link.get());
      in_pin->links.push_back(link.get());
      return link;
  }
  ```
  Este é o **único** teste/helper que o plano autoriza editar na Task 1. Se qualquer outro teste precisar de edição para compilar ou passar, pare: é sinal de que a migração mudou comportamento.
- Test: `ficsit-companion/tests/test_pin.cpp`, `ficsit-companion/tests/test_graph_model.cpp`

**Interfaces:**
- Consumes: nada (primeira tarefa).
- Produces:
  - `std::vector<Link*> Pin::links` — os links incidentes ao pino, na ordem de criação.
  - `Link* Pin::SoleLink() const` — `links.front()` se houver exatamente um link, `nullptr` se não houver nenhum. **Retorna `nullptr` se houver mais de um** (é um erro de programação chamar isso num pino com fan-out; o call site deve iterar `links`).
  - `FractionalNumber Link::current_rate` — a taxa da aresta; `0/1` no construtor.

- [ ] **Step 1: Escreva o teste que falha (Pin começa sem links, acumula, e `SoleLink` só vale com um)**

Adicione ao fim de `ficsit-companion/tests/test_pin.cpp`:

```cpp
/// @test   A Pin starts with no link, accumulates them, and SoleLink() only yields a Link when
///         there is exactly one.
/// @covers Pin::links, Pin::SoleLink. Guards the invariant the snapshot and save-import code
///         relies on: there a pin never carries more than one link, and SoleLink() is the
///         honest accessor for that.
TEST_CASE("Pin::links starts empty and SoleLink reflects arity", "[pin]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);

    Node* producer = AddCraftNode(g);   // one output
    Node* consumer_a = AddCraftNode(g); // one input
    Node* consumer_b = AddCraftNode(g);

    Pin* out = producer->outs[0].get();
    REQUIRE(out->links.empty());
    REQUIRE(out->SoleLink() == nullptr);

    float error_time = 0.0f;
    g.CreateLink(out, consumer_a->ins[0].get(), false, error_time, 1.0f);
    REQUIRE(out->links.size() == 1);
    REQUIRE(out->SoleLink() == out->links[0]);

    g.CreateLink(out, consumer_b->ins[0].get(), false, error_time, 1.0f);
    REQUIRE(out->links.size() == 2);
    REQUIRE(out->SoleLink() == nullptr); // fan-out: the call site must iterate links
}
```

Se `AddCraftNode` não existir em `graph_test_helpers.hpp`, use o helper que os testes vizinhos de `test_graph_model.cpp` já usam para montar um nó com pinos, e ajuste os nomes. Leia `ficsit-companion/tests/graph_test_helpers.hpp` antes de escrever o teste.

- [ ] **Step 2: Rode e confirme que falha**

```bash
cmake --build build --config Release --target fc-tests
```

Esperado: **erro de compilação** — `'class Pin' has no member named 'links'`. Isso conta como falha.

- [ ] **Step 3: Mude `Pin` e `Link`**

Em `include/domain/graph/pin.hpp`, adicione `#include <vector>` e substitua o membro `link`:

```cpp
    // A pin can carry several links in the Production Planner (fan-out / fan-in);
    // the pin's rate is the sum of its links' rates. Factory Snapshot, Vehicle Map
    // and the .sav importer never create more than one.
    std::vector<Link*> links;

    /// @brief This pin's only link, or nullptr if it has none — or more than one.
    ///        A pin with fan-out has no single "the" link: code that must handle
    ///        fan-out iterates `links` instead of calling this.
    Link* SoleLink() const { return links.size() == 1 ? links.front() : nullptr; }
```

Em `include/domain/graph/link.hpp`, adicione o include de `FractionalNumber` e o membro:

```cpp
    /// @brief The rate flowing along this edge. On a single-link pin it equals the rate
    ///        of both ends; on a pin with fan-out, the links' rates sum to the pin's rate.
    FractionalNumber current_rate;
```

Inicialize `current_rate(0, 1)` na lista de inicialização do construtor em `src/domain/graph/link.cpp`, e remova `link(nullptr)` da lista de inicialização de `Pin` em `src/domain/graph/pin.cpp:8`.

- [ ] **Step 4: Migre `GraphModel`**

`src/domain/graph/graph_model.cpp:108-109` — em `CreateLink`, troque a atribuição por append:

```cpp
    start->links.push_back(created);
    end->links.push_back(created);
```

`src/domain/graph/graph_model.cpp:279-311` — em `DeleteLink`, troque `pin->link = nullptr` por remoção do vetor, e **só rode a limpeza de item/taxa quando o pino ficou sem nenhum link**:

```cpp
        Link* dead = it->get();
        auto detach = [dead](Pin* pin) -> bool {
            if (pin == nullptr) return false;
            auto& ls = pin->links;
            ls.erase(std::remove(ls.begin(), ls.end(), dead), ls.end());
            return ls.empty();
        };

        if (Pin* start = (*it)->start; start != nullptr)
        {
            // The cleanup below only applies when this was the pin's last link: with
            // fan-out, the remaining links still type and feed the pin.
            if (detach(start))
            {
                if (start->node->IsOrganizer())
                {
                    static_cast<OrganizerNode*>(start->node)->RemoveItemIfNotForced();
                }
                else if (start->node->IsLogistics())
                {
                    start->item = nullptr;
                    start->current_rate = 0;
                }
            }
        }
        if (Pin* end = (*it)->end; end != nullptr)
        {
            if (detach(end))
            {
                if (end->node->IsOrganizer())
                {
                    static_cast<OrganizerNode*>(end->node)->RemoveItemIfNotForced();
                }
                else if (end->node->IsSink())
                {
                    end->item = nullptr;
                    end->current_rate = 0;
                }
                else if (end->node->IsLogistics())
                {
                    end->item = nullptr;
                    end->current_rate = 0;
                }
            }
        }
        links.erase(it);
```

`src/domain/graph/graph_model.cpp:322-335` — em `DeleteNode`, `DeleteLink` muta `p->links` enquanto você itera. Copie os ids antes:

```cpp
        std::vector<ax::NodeEditor::LinkId> to_delete;
        for (auto& p : (*it)->ins)
        {
            for (Link* l : p->links) to_delete.push_back(l->id);
        }
        for (auto& p : (*it)->outs)
        {
            for (Link* l : p->links) to_delete.push_back(l->id);
        }
        for (auto lid : to_delete) DeleteLink(lid);
```

- [ ] **Step 5: Migre o resto dos call sites, mecanicamente**

O padrão é sempre um destes dois:

- Onde o código **decide se o pino está ligado** (`if (p->link != nullptr)` / `== nullptr`) → `if (!p->links.empty())` / `if (p->links.empty())`.
- Onde o código **atravessa o link** (`p->link->start`, `p->link->end`) → itere:
  ```cpp
  for (Link* l : p->links) { Pin* far = p->direction == ax::NodeEditor::PinKind::Input ? l->start : l->end; /* ... */ }
  ```

Nos arquivos de snapshot (`snapshot_bypass.cpp`, `rate_propagation.cpp`), de importação (`sav_import.cpp`) e nos pontos de UI que só querem "o" link (`production_app.cpp:1716-1726`, ordenação dos pinos), **use `SoleLink()`**: ali o pino tem no máximo um link por construção, e `SoleLink()` documenta isso. Onde `SoleLink()` for usado, mantenha o `nullptr`-check que já existe.

`src/domain/nodes/organizer_nodes.cpp:109-123` (`RemoveItemIfNotForced`) e `src/domain/graph/graph_item_resolve.cpp` (travessia) precisam iterar `links`, porque um organizer/craft **pode** ter fan-out no planner.

`src/domain/solver/rate_solver_seed.cpp` e `rate_solver_system.cpp`: nesta tarefa faça a migração **mínima** que preserva o comportamento — onde o código faz `pin->link`, itere `pin->links` empurrando **todas** as pontas para a fila (`pins_to_propagate`) e marcando o `flow` de cada link. Em `rate_solver_system.cpp:36-48`, gere uma equação de igualdade por link (`start == end`) para cada link de cada pino relevante. Isso **ainda está errado para multi-link** (forçaria os dois consumidores a taxas iguais) — a Task 2 conserta. Aqui só precisa continuar correto para 1 link.

`src/app/production_app.cpp:2829` e `:2861`: mantenha a rejeição, só reescreva o teste:
```cpp
                    (!both_plugs && (!start_pin->links.empty() || !end_pin->links.empty())) ||
```
```cpp
            if (input_pin == nullptr || !input_pin->links.empty() || IsVehiclePlug(input_pin))
```

- [ ] **Step 6: Rode a suíte inteira**

```bash
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure
```

Esperado: **tudo verde, sem editar nenhum teste existente.** Um teste existente que quebre significa que a migração mudou comportamento — conserte o código, não o teste.

- [ ] **Step 7: Registre em `PROGRESS.md`**

Acrescente uma seção no topo descrevendo: `Pin::links` e `Link::current_rate` no lugar de `Pin::link`; migração mecânica; nenhuma mudança de comportamento; solver ainda trata multi-link errado (Task 2). Sem git.

---

### Task 2: Solver — variável por link e balanço por pino

Aqui o multi-link passa a **funcionar**: taxa do pino = soma dos links, e o "somar a demanda" aparece.

**Files:**
- Modify: `ficsit-companion/src/domain/solver/rate_solver_internal.hpp`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_seed.cpp`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_variables.cpp`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_system.cpp`
- Modify: `ficsit-companion/src/domain/solver/rate_solver_apply.cpp`
- Test: `ficsit-companion/tests/test_rate_solver.cpp`

**Interfaces:**
- Consumes: `Pin::links`, `Link::current_rate` (Task 1).
- Produces:
  - `SeedResult::relevant_links` — `std::unordered_set<const Link*>`, todos os links incidentes a um pino relevante.
  - `VariableMapping::link_variable_index` — `std::unordered_map<const Link*, std::size_t>`.
  - `VariableMapping::variable_of_link` — `std::unordered_map<std::size_t, const Link*>`, o inverso; usado em `ApplyResults` para saber que uma variável livre é de link (o `reversed_variable_map` dela é `nullptr`, igual ao total de rota `T`).
  - Após um `Solve` bem-sucedido, todo `Link` relevante tem `current_rate` gravado.

- [ ] **Step 1: Escreva os testes que falham**

A `CraftFixture` do arquivo (`test_rate_solver.cpp:218-230`) já dá uma receita `30 ore → 20 plate`. Precisamos também de um consumidor de plate, então acrescente esta fixture logo depois dela:

```cpp
namespace
{
    // A 30 ore -> 20 plate producer, and a 20 plate -> 10 rod consumer.
    // Two consumers will pull from the SAME producer output pin: that is the fan-out.
    struct FanOutFixture
    {
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        Item rod{ "Iron Rod", "", 3 };

        std::vector<CountedItem> prod_ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> prod_outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe producer{ prod_ins, prod_outs, &building, false, 4.0, "Recipe_IronPlate_C" };

        std::vector<CountedItem> cons_ins{ CountedItem(&plate, FractionalNumber(20, 1)) };
        std::vector<CountedItem> cons_outs{ CountedItem(&rod, FractionalNumber(10, 1)) };
        Recipe consumer{ cons_ins, cons_outs, &building, false, 4.0, "Recipe_IronRod_C" };
    };
}

/// @test   Fan-out: with two consumers on the same output pin, pinning one consumer's demand
///         leaves the other alone — the producer's output becomes the sum of both demands, and
///         each link carries its own share.
/// @covers RateSolver::Solve on a multi-link pin: the pin balance equation (sum of the links
///         equals the pin's rate) plus the existing free-variable rule "keep the current value".
///         This is the case that motivated the feature (Iron Rod feeding Screw and Rotor).
TEST_CASE("RateSolver: fan-out sums demand into the producer", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_b = static_cast<CraftNode*>(nodes.back().get());

    // Both consumers hang off the SAME producer output pin.
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_a->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_b->ins[0].get()));
    Link* link_b = links.back().get();

    REQUIRE(producer->outs[0]->links.size() == 2);

    float error_time = 0.0f;

    // 1) Consumer A asks for 40 plate/min. B asks for nothing yet; its variable is free and
    //    the solver holds it at 0.
    REQUIRE(RateSolver::Solve(nodes, links, consumer_a->ins[0].get(),
                              FractionalNumber(40, 1), error_time, 1.0f));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(40, 1));

    // 2) Now consumer B asks for 20 plate/min. A's variable is free and the solver holds it at
    //    40 — that is where "production ramps up" comes from, instead of "A gets squeezed".
    REQUIRE(RateSolver::Solve(nodes, links, consumer_b->ins[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(error_time == 0.0f);

    REQUIRE(consumer_a->ins[0]->current_rate == FractionalNumber(40, 1)); // not squeezed
    REQUIRE(consumer_b->ins[0]->current_rate == FractionalNumber(20, 1));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1));  // 40 + 20
    // The producer scaled along with it: 60 plate needs 90 ore (the 30/20 recipe ratio).
    REQUIRE(producer->ins[0]->current_rate == FractionalNumber(90, 1));

    // Each edge carries its own share...
    REQUIRE(link_a->current_rate == FractionalNumber(40, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(20, 1));
    // ...and the central invariant holds: a pin's rate is the sum of its links' rates.
    REQUIRE(link_a->current_rate + link_b->current_rate == producer->outs[0]->current_rate);
}

/// @test   Fan-in: two producers on the same input pin sum into the consumer.
/// @covers The mirror of the case above — the same balance equation, on an input pin (an
///         implicit merger). Proves the balance is not a special case of output pins.
TEST_CASE("RateSolver: fan-in sums supply into the consumer", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer_b = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer = static_cast<CraftNode*>(nodes.back().get());

    // Both producers hang off the SAME consumer input pin.
    links.push_back(MakeLink(id_gen(), producer_a->outs[0].get(), consumer->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer_b->outs[0].get(), consumer->ins[0].get()));
    Link* link_b = links.back().get();

    REQUIRE(consumer->ins[0]->links.size() == 2);

    float error_time = 0.0f;
    REQUIRE(RateSolver::Solve(nodes, links, producer_a->outs[0].get(),
                              FractionalNumber(30, 1), error_time, 1.0f));
    REQUIRE(RateSolver::Solve(nodes, links, producer_b->outs[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));

    REQUIRE(producer_a->outs[0]->current_rate == FractionalNumber(30, 1));
    REQUIRE(producer_b->outs[0]->current_rate == FractionalNumber(20, 1));
    REQUIRE(consumer->ins[0]->current_rate == FractionalNumber(50, 1)); // 30 + 20
    REQUIRE(link_a->current_rate == FractionalNumber(30, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(20, 1));
}

```

(O invariante "taxa do pino = soma das taxas dos links" já é asserido dentro do teste de fan-out, na última linha — não precisa de um caso separado.)

- [ ] **Step 2: Rode e confirme que falham**

```bash
./build/ficsit-companion/Release/fc-tests.exe "[multilink]"
```

Esperado: FAIL. O fan-out deve falhar mostrando o producer em 20 ou 40 (não 60) — a equação de igualdade por link da Task 1 força os dois consumidores à mesma taxa.

- [ ] **Step 3: Semeie todos os links (`rate_solver_seed.cpp`)**

Em `SeedResult`, adicione:

```cpp
        std::unordered_set<const Link*> relevant_links;
```

Onde o seed empurra a ponta distante de um link, ele agora empurra **todas** as pontas e registra cada link. Substitua o helper `BeltFarEnd` por:

```cpp
    // The far ends of every link on a pin (the pins feeding it, or the ones it feeds).
    // Empty when the pin has no link.
    std::vector<const Pin*> BeltFarEnds(const Pin* pin)
    {
        std::vector<const Pin*> out;
        out.reserve(pin->links.size());
        for (const Link* l : pin->links)
        {
            out.push_back(pin->direction == ax::NodeEditor::PinKind::Input ? l->start : l->end);
        }
        return out;
    }
```

E em cada um dos pontos que hoje fazem `if (p->link != nullptr) { pins_to_propagate.push(...); p->link->flow = ...; }` (linhas 44-51, 76-84, 110-117, 126-134, 155-162, 205-213 do arquivo original), troque por um laço sobre `p->links` que empurra cada ponta distante, marca `flow` de cada link e faz `seed.relevant_links.insert(l)`. Em `:222`, `p->link != nullptr` vira `!p->links.empty()`.

- [ ] **Step 4: Uma variável por link (`rate_solver_variables.cpp`)**

Em `VariableMapping` (`rate_solver_internal.hpp`):

```cpp
        std::unordered_map<const Link*, std::size_t> link_variable_index;
        std::unordered_map<std::size_t, const Link*> variable_of_link;
```

Em `AssignVariables`, **depois** dos dois laços que criam as variáveis de pino e **antes** do bloco que aloca os totais de rota `T` (para não mexer na ordem de pivô que o código já depende), aloque uma variável por link relevante:

```cpp
        // One variable per link: the edge's rate. On a single-link pin the balance equation
        // pins it to the pin's rate and the system collapses to what it was before; on a pin
        // with fan-out it is the degree of freedom representing the split between consumers.
        for (const Link* l : seed.relevant_links)
        {
            vars.link_variable_index[l] = vars.num_variables;
            vars.variable_of_link[vars.num_variables] = l;
            vars.num_variables += 1;
        }
```

`reversed_variable_map` continua `nullptr` nesses índices — é assim que `ApplyResults` distingue variável de link de variável de pino, exatamente como já faz com `T`.

- [ ] **Step 5: Balanço por pino (`rate_solver_system.cpp`)**

Remova o bloco `processed_links` / equação de igualdade (linhas 36-48). No lugar, dentro do mesmo laço `for (const Pin* pin : seed.relevant_pins)`:

```cpp
            // Pin balance: the sum of its links' rates is the pin's rate.
            //   sum(link_vars) - coef(pin) * var(pin) = 0
            // With a single link this reduces to link == pin on both ends, i.e. the old
            // start == end equality equation. With several, it is the fan-out split.
            if (!pin->links.empty())
            {
                std::vector<FractionalNumber> equation(vars.num_variables);
                for (const Link* l : pin->links)
                {
                    equation[vars.link_variable_index.at(l)] += 1;
                }
                const std::pair<size_t, FractionalNumber>& pin_variable = vars.associated_variable_index.at(pin);
                equation[pin_variable.first] -= pin_variable.second;
                sys.equations_coefficients.push_back(equation);
                sys.constants.push_back(0);
            }
```

Use `+=` e `-=` (não `=`): num Craft, vários pinos compartilham a mesma variável, e um `=` sobrescreveria a contribuição de um pino irmão.

- [ ] **Step 6: Variável de link livre e gravação da taxa (`rate_solver_apply.cpp`)**

No laço de variáveis livres, `vars.reversed_variable_map[free_index]` é `nullptr` tanto para `T` quanto para uma variável de link. Antes do ramo que trata `T` (o `if (pin == nullptr)` de `:66`), trate o link:

```cpp
        const auto link_it = vars.variable_of_link.find(free_index);
        if (link_it != vars.variable_of_link.end())
        {
            // Free link: keep the rate the edge already carried. (Proportional re-splitting
            // when the shared pin's total changes comes in Task 2b.)
            sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
            sys.equations_coefficients.back()[free_index] = 1;
            sys.constants.push_back(link_it->second->current_rate);
        }
        else if (pin == nullptr) { /* ... the existing handling of the route total T ... */ }
```

Atenção ao `processed_pins.insert(pin)` no fim do laço: com `pin == nullptr` para links, todas as variáveis de link colidiriam na mesma chave. Troque o conjunto `processed_pins` por um `std::unordered_set<int> processed_variables` indexado por `free_index`, que é único por variável — e ajuste o ramo "já vi este pino" (`:214`) de acordo.

Depois da substituição regressiva, grave a taxa em cada link relevante, junto com as taxas dos pinos:

```cpp
    for (auto& l : in.links)
    {
        const auto it = vars.link_variable_index.find(l.get());
        if (it != vars.link_variable_index.end())
        {
            l->current_rate = solution[it->second];
        }
    }
```

O check de solução negativa (`:282-302`) deve valer para link também: uma aresta com taxa negativa é um estado inválido. Estenda-o para varrer `vars.link_variable_index` e marcar `error` nos dois pinos da aresta.

- [ ] **Step 7: Rode os testes novos, depois a suíte inteira**

```bash
./build/ficsit-companion/Release/fc-tests.exe "[multilink]"
ctest --test-dir build -C Release --output-on-failure
```

Esperado: `[multilink]` PASS, e **tudo o mais verde sem editar nenhum teste existente**. É aqui que se prova que a generalização não mexeu no caso de 1 link.

- [ ] **Step 8: Registre em `PROGRESS.md`** (sem git)

---

### Task 2b: Repartição proporcional quando a taxa do pino compartilhado muda

Cenário: Rod divide 40 para Screw e 20 para Rotor. Você edita a saída do Rod para 90. A Task 2 deixa uma variável de link livre e a fixa no valor antigo, o que dá um resultado torto (um ramo absorve toda a diferença). O certo é manter a proporção: 60 e 30.

Isto é exatamente a regra que `CustomSplitter`/`Merger` já usam para os seus multi-pinos (`rate_solver_apply.cpp:135-208`), com `single_pin` := o pino, e `multi_pin` := os links dele.

**Files:**
- Modify: `ficsit-companion/src/domain/solver/rate_solver_apply.cpp`
- Test: `ficsit-companion/tests/test_rate_solver.cpp`

**Interfaces:**
- Consumes: `VariableMapping::link_variable_index`, `variable_of_link`, `SeedResult::relevant_links` (Task 2).
- Produces: nada novo na API.

- [ ] **Step 1: Escreva o teste que falha**

```cpp
Monte o mesmo fan-out 40/20 do teste da Task 2 (mesma `FanOutFixture`, mesmos dois `MakeLink` no pino de saída do produtor, mesmos dois `Solve` que estabelecem 40 e 20). A partir desse estado:

```cpp
/// @test   Raising a fan-out pin's rate re-splits the new total in the branches' prior ratio.
/// @covers The free-link-variable rule. Without it one branch absorbs the whole difference:
///         the solver would hold link_a at its old 40 and dump the remaining 50 on link_b.
TEST_CASE("RateSolver: raising a fan-out pin keeps the branch ratio", "[rate_solver][multilink]")
{
    // ... build the 40 / 20 fan-out (total 60), as in the Task 2 test ...

    // Now drive the shared output pin itself to 90/min.
    REQUIRE(RateSolver::Solve(nodes, links, producer->outs[0].get(),
                              FractionalNumber(90, 1), error_time, 1.0f));

    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(90, 1));
    // The 2:1 ratio is preserved: 90 splits as 60 / 30, not 40 / 50.
    REQUIRE(link_a->current_rate == FractionalNumber(60, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(30, 1));
    REQUIRE(consumer_a->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(consumer_b->ins[0]->current_rate == FractionalNumber(30, 1));
}

/// @test   A locked branch stays out of the re-split: it holds its locked rate and the other
///         branches share whatever is left.
/// @covers How the lock interacts with fan-out. A locked branch folds into the equation's
///         constant, exactly as a locked pin does on a CustomSplitter.
TEST_CASE("RateSolver: a locked branch keeps its rate under fan-out", "[rate_solver][multilink]")
{
    // ... build the 40 / 20 fan-out (total 60), as in the Task 2 test ...

    consumer_a->ins[0]->SetLocked(true); // pin branch A at 40

    REQUIRE(RateSolver::Solve(nodes, links, producer->outs[0].get(),
                              FractionalNumber(90, 1), error_time, 1.0f));

    REQUIRE(link_a->current_rate == FractionalNumber(40, 1)); // locked, unchanged
    REQUIRE(link_b->current_rate == FractionalNumber(50, 1)); // takes the whole remainder
}
```

- [ ] **Step 2: Rode e confirme que falham**

```bash
./build/ficsit-companion/Release/fc-tests.exe "[multilink]"
```

Esperado: o primeiro falha com `link_a == 40, link_b == 50` (o valor antigo mantido em vez do rateio).

- [ ] **Step 3: Implemente o rateio**

No ramo de variável de link livre da Task 2, em vez de fixar no valor atual: identifique o pino "dono" do rateio — o pino do link cujo `links.size() > 1` (se as duas pontas tiverem fan-out, use a ponta de saída, `l->start`). Se nenhuma ponta tem fan-out, mantenha o valor atual (é o caso de 1 link, e o comportamento da Task 2 já está certo).

Com o pino dono `P`, gere uma equação por link **não travado e não já restringido** de `P`, com a mesma forma da regra do `CustomSplitter`:

```
x_l - multiplier * var(P) = -multiplier * sum_locked

where multiplier = current_rate(l) / sum of the rates of P's unlocked links
                   (or 1/n when that sum is zero — split evenly)
      sum_locked = sum of the rates of P's locked links
```

Um link é "travado" quando o pino da ponta distante está travado (`GetLocked()`), que é como o cadeado se manifesta num ramo. Gere as equações para **todos** os links elegíveis de `P` de uma vez, não só o livre — é o que o código do `CustomSplitter` faz (`:172-174`), e pela mesma razão: constranger um ramo só desbalancearia o nó.

- [ ] **Step 4: Rode os testes e a suíte**

```bash
./build/ficsit-companion/Release/fc-tests.exe "[multilink]"
ctest --test-dir build -C Release --output-on-failure
```

Esperado: tudo PASS.

- [ ] **Step 5: Registre em `PROGRESS.md`** (sem git)

---

### Task 3: Cadeado — o lock não atravessa um fan-out

`Pin::SetLocked` (`pin.cpp:26-33`) propaga o cadeado pelo link: travar um lado trava o outro. Faz sentido com um link (as duas pontas têm a mesma taxa, travar uma trava a outra). Com fan-out, não: travar a saída do Rod em 60 fixa o **total**, não fixa que o Screw leva 40.

**Files:**
- Modify: `ficsit-companion/src/domain/graph/pin.cpp:26-33`
- Modify: `ficsit-companion/src/domain/graph/graph_model.cpp:129-133`
- Test: `ficsit-companion/tests/test_pin.cpp`

**Interfaces:**
- Consumes: `Pin::links`, `Pin::SoleLink()` (Task 1).
- Produces: nada novo na API.

- [ ] **Step 1: Escreva o teste que falha**

```cpp
/// @test   Locking a pin with fan-out does not lock its consumers: the lock fixes the pin's
///         total, not how that total divides between the branches.
/// @covers Pin::SetLocked on a multi-link pin. The single-link case must keep propagating, so
///         the test asserts both halves — the new restriction and the untouched old behaviour.
TEST_CASE("Pin::SetLocked does not cross a fan-out", "[pin][multilink]")
{
    FanOutFixture fx;
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto id_gen = [&g] { return g.GetNextId(); };
    float error_time = 0.0f;

    // Fan-out: one producer, two consumers on the same output pin.
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_a = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_b = static_cast<CraftNode*>(g.nodes.back().get());
    g.CreateLink(producer->outs[0].get(), consumer_a->ins[0].get(), false, error_time, 1.0f);
    g.CreateLink(producer->outs[0].get(), consumer_b->ins[0].get(), false, error_time, 1.0f);

    producer->outs[0]->SetLocked(true);

    REQUIRE(producer->outs[0]->GetLocked());
    REQUIRE_FALSE(consumer_a->ins[0]->GetLocked()); // the lock did not cross the fan-out
    REQUIRE_FALSE(consumer_b->ins[0]->GetLocked());

    // A plain single-link edge still propagates the lock, exactly as before.
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* lone_producer = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* lone_consumer = static_cast<CraftNode*>(g.nodes.back().get());
    g.CreateLink(lone_producer->outs[0].get(), lone_consumer->ins[0].get(), false, error_time, 1.0f);

    lone_producer->outs[0]->SetLocked(true);
    REQUIRE(lone_consumer->ins[0]->GetLocked());
}
```

- [ ] **Step 2: Rode e confirme que falha**

```bash
./build/ficsit-companion/Release/fc-tests.exe "[pin]"
```

- [ ] **Step 3: Implemente**

Em `pin.cpp`, o bloco `if (link != nullptr)` vira:

```cpp
    // The lock only crosses an edge when that edge is the sole link on both sides: only then
    // do the two ends necessarily carry the same rate, so locking one locks the other. Under
    // fan-out the lock fixes the pin's total, not how it divides between branches, so it does
    // not propagate.
    if (Link* sole = SoleLink(); sole != nullptr)
    {
        Pin* linked_pin = direction == ax::NodeEditor::PinKind::Input ? sole->start : sole->end;
        if (linked_pin->SoleLink() == sole && linked_pin->locked != b)
        {
            linked_pin->SetLocked(b);
        }
    }
```

Em `graph_model.cpp:129-133`, a sincronização de lock na criação do link (`if (start->GetLocked() || end->GetLocked()) { ambos travam; }`) só deve rodar quando o link novo é o único dos dois lados:

```cpp
    if (start->links.size() == 1 && end->links.size() == 1 &&
        (start->GetLocked() || end->GetLocked()))
    {
        start->SetLocked(true);
        end->SetLocked(true);
    }
```

- [ ] **Step 4: Rode os testes e a suíte** — `[pin]` PASS, tudo verde.

- [ ] **Step 5: Registre em `PROGRESS.md`** (sem git)

---

### Task 4: `CreateLink` escolhe o pino restringido

Hoje `CreateLink` empurra da origem (`graph_model.cpp:110-127`: restringe `start` no valor que ele já tem). Num fan-out isso achata o consumidor novo — o Rod continua em 40 e o Rotor recebe 40 em vez de 20, ou o Screw cai. A regra correta é puxar do lado que não está fazendo o fan.

**Files:**
- Modify: `ficsit-companion/src/domain/graph/graph_model.cpp:110-127`
- Test: `ficsit-companion/tests/test_graph_model.cpp`

**Interfaces:**
- Consumes: solver multi-link (Task 2), lock (Task 3).
- Produces: nada novo na API — `CreateLink` mantém a assinatura.

- [ ] **Step 1: Escreva o teste que falha**

Reutilize a `FanOutFixture` da Task 2 (mova-a para `graph_test_helpers.hpp` se preferir compartilhá-la entre os dois arquivos de teste; se mover, mantenha o mesmo nome e campos).

```cpp
/// @test   When a new consumer is wired to an output pin that already has a link, the new
///         consumer is what drives the solve: production ramps up to the sum, instead of the
///         new branch being squeezed into whatever rate the source already carried.
/// @covers The constraint-pin choice in GraphModel::CreateLink. This is the step that turns the
///         multi-link solver (Task 2) into the "sum the demand" behaviour the user sees: without
///         it CreateLink pushes from the source and the Rotor would receive 40 instead of 20.
TEST_CASE("GraphModel::CreateLink pulls from the new end on a fan-out", "[graph_model][multilink]")
{
    FanOutFixture fx;
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto id_gen = [&g] { return g.GetNextId(); };

    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* screw = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* rotor = static_cast<CraftNode*>(g.nodes.back().get());

    float error_time = 0.0f;

    // Established: the producer feeds the screw at 40/min.
    g.CreateLink(producer->outs[0].get(), screw->ins[0].get(), true, error_time, 1.0f);
    RateSolver::Solve(g.nodes, g.links, screw->ins[0].get(), FractionalNumber(40, 1), error_time, 1.0f);
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(40, 1));

    // The rotor already demands 20/min before being wired up (the user configured it first).
    RateSolver::Solve(g.nodes, g.links, rotor->ins[0].get(), FractionalNumber(20, 1), error_time, 1.0f);
    REQUIRE(rotor->ins[0]->current_rate == FractionalNumber(20, 1));

    // Second link on the SAME output pin: this is where the constraint rule decides everything.
    g.CreateLink(producer->outs[0].get(), rotor->ins[0].get(), true, error_time, 1.0f);

    REQUIRE(error_time == 0.0f);
    REQUIRE(producer->outs[0]->links.size() == 2);
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1)); // ramped up: 40 + 20
    REQUIRE(screw->ins[0]->current_rate == FractionalNumber(40, 1));     // not squeezed
    REQUIRE(rotor->ins[0]->current_rate == FractionalNumber(20, 1));     // kept its demand
}
```

- [ ] **Step 2: Rode e confirme que falha**

- [ ] **Step 3: Implemente**

Em `CreateLink`, depois do `push_back` nos dois pinos e antes do solve, escolha o pino de restrição:

```cpp
    // Which end drives the solve. When one side already had links, this new link is a fan-out /
    // fan-in branch: the *new* end dictates the rate (the consumer that was just wired up, or
    // the producer that was just added), and the fanning pin absorbs the sum. With no links on
    // either side, it is the usual push from the source.
    const bool start_fans = start->links.size() > 1;
    const bool end_fans = end->links.size() > 1;
    const Pin* constraint = nullptr;
    if (start_fans) constraint = end;        // source fans out: pull the destination's demand
    else if (end_fans) constraint = start;   // destination fans in: push the source's supply
```

e o solve fica:

```cpp
    if (trigger_update && (constraint != nullptr || start->current_rate != end->current_rate))
    {
        const Pin* solve_pin = constraint != nullptr ? constraint : start;
        try
        {
            if (!RateSolver::Solve(nodes, links, solve_pin, solve_pin->current_rate, error_time, error_flow_duration))
            { DeleteLink(created->id); return; }
        }
        catch (const std::runtime_error&)
        {
            DeleteLink(created->id);
            fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
            error_time = error_flow_duration;
            return;
        }
    }
```

Note que `DeleteLink(links.back()->id)` do código original vira `DeleteLink(created->id)`: com o solve podendo criar/remover coisas, `links.back()` não é mais garantidamente este link.

Se as duas pontas fanam, `start_fans` ganha e o destino é restringido — a ponta nova é sempre o destino nesse caso, porque `real_end` é o input.

- [ ] **Step 4: Rode os testes e a suíte** — tudo verde.

- [ ] **Step 5: Registre em `PROGRESS.md`** (sem git)

---

### Task 5: Persistência — taxa por link, e saves antigos abrem idênticos

**Files:**
- Modify: `ficsit-companion/src/infra/persistence/session_serializer.cpp:77-87` (salvar), `:141+` (carregar)
- Modify: `ficsit-companion/src/domain/nodes/group_node.cpp:54-81` (carregar), `:200-211` (salvar)
- Test: `ficsit-companion/tests/test_session_serializer.cpp`, `ficsit-companion/tests/test_group_node.cpp`

**Interfaces:**
- Consumes: `Link::current_rate` (Task 1).
- Produces: no JSON, cada entrada de `links` ganha `"rate": "<num>/<den>"` — use a mesma serialização de `FractionalNumber` que os pinos já usam no arquivo (leia como `Node::Serialize` grava `current_rate` e siga o mesmo formato, seja string ou par de inteiros).

- [ ] **Step 1: Escreva os testes que falham**

```cpp
/// @test   A fan-out graph survives the round-trip: the two branches come back with their
///         distinct rates, exactly as they were saved.
/// @covers SessionSerializer with Link::current_rate. Without the field the two branches would
///         come back at the same rate (or zero), and the graph saved would not be the graph
///         loaded.
TEST_CASE("SessionSerializer round-trips fan-out link rates", "[session_serializer][multilink]")
{
    // Build the 40 / 20 fan-out (as in the Task 2 test), Serialize(), then Deserialize() into a
    // fresh GraphModel and assert the two links came back at 40 and 20.
}

/// @test   A save written before this feature (no "rate" field on links) loads unchanged: every
///         pin there carries at most one link, so the edge's rate is the pin's rate.
/// @covers Backward compatibility. This test is the contract with users' existing .fcs files.
TEST_CASE("SessionSerializer loads a pre-multilink save unchanged", "[session_serializer][multilink]")
{
    // Feed Deserialize() a hand-written JSON literal with no "rate" on its links, and assert
    // each link ended up carrying its pin's rate.
}
```

Os dois corpos acima estão descritos, não escritos. Ao implementar, monte-os com o mesmo padrão dos casos que já existem em `test_session_serializer.cpp` — leia o arquivo antes.

- [ ] **Step 2: Rode e confirme que falham**

- [ ] **Step 3: Implemente**

Ao salvar (`session_serializer.cpp:77-87` e `group_node.cpp:200-211`), acrescente `{ "rate", <l->current_rate serializado> }` ao objeto do link.

Ao carregar, depois de criar o `Link` e antes de sair do laço:

```cpp
        // Saves older than this feature carry no "rate": there every pin holds at most one
        // link, so the edge's rate is the pin's rate, and that is what we rebuild.
        if (l.contains("rate"))
        {
            link->current_rate = <deserialize l["rate"]>;
        }
        else
        {
            link->current_rate = end->current_rate;
        }
```

(Use `end` — o pino de entrada — e não `start`: num grafo legado os dois são iguais, e o de entrada é o que existe em qualquer caso.)

- [ ] **Step 4: Rode os testes e a suíte** — tudo verde, e os testes de serialização existentes **sem edição**.

- [ ] **Step 5: Registre em `PROGRESS.md`** (sem git)

---

### Task 6: UI — liberar o arrasto, recolorir e mostrar a taxa da aresta

Só agora o usuário consegue de fato fazer o fan-out na tela.

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp:2829` (a rejeição), `:2861` (spawn de nó), `:2741-2768` (`RenderLinks`)

**Interfaces:**
- Consumes: tudo das tarefas 1-5.
- Produces: nada — é a camada de UI.

- [ ] **Step 1: Libere o arrasto**

Em `DragLink` (`:2823-2833`), remova a cláusula que rejeita pino ocupado:

```cpp
                    (!both_plugs && (!start_pin->links.empty() || !end_pin->links.empty())) ||
```

Deixe as outras rejeições em pé (mesmo pino, mesma direção, mesmo nó, plug solto, item incompatível, dois lados travados com taxas diferentes). **Adicione uma rejeição nova**: dois pinos já ligados entre si não podem ser ligados de novo — sem isso o usuário cria links duplicados sobrepostos:

```cpp
                const bool already_linked = [&] {
                    for (const Link* l : start_pin->links)
                    {
                        if (l->start == end_pin || l->end == end_pin) return true;
                    }
                    return false;
                }();
```

e some `already_linked` à lista de condições de rejeição.

Em `:2861`, deixe o spawn de nó a partir de um pino ocupado funcionar (arrastar de um pino já ligado para o vazio abre o menu de nó novo):

```cpp
            if (input_pin == nullptr || IsVehiclePlug(input_pin))
```

- [ ] **Step 2: Recolore o link em `RenderLinks` (`:2746`)**

`link->start->current_rate != link->end->current_rate` deixa de valer — num fan-out as pontas *devem* diferir. A condição de erro passa a ser: a taxa de um pino não bate com a soma dos seus links.

```cpp
        // A pin is consistent when the sum of its links' rates equals its own rate. On a
        // single-link pin that is the old equality; under fan-out it is the only check that
        // makes sense, because the link's two ends legitimately differ.
        auto pin_balanced = [](const Pin* p) {
            FractionalNumber sum(0, 1);
            for (const Link* l : p->links) sum += l->current_rate;
            return sum == p->current_rate;
        };
        if (!pin_balanced(link->start) || !pin_balanced(link->end))
        {
            link_color = ImColor(1.0f, 0.0f, 0.0f); // Red
        }
```

O resto da cadeia (`else if` do Sink com item nulo → laranja; `else` → verde) fica como está.

- [ ] **Step 3: Mostre a taxa da aresta no tooltip**

`RenderLinks` já tem um bloco de tooltip para link sob o mouse, mas só sob `settings.show_debug_ids` (`:2770-2786`). Acrescente um tooltip **sempre ativo** para o link sob o cursor, com a taxa da aresta. Isso resolve o único caso em que o número não é dedutível da tela: um fan-out alimentando um fan-in direto, onde nenhum pino sozinho revela a divisão.

Formate a taxa com o mesmo helper que os pinos usam para exibir `current_rate` (procure em `production_app.cpp` como a taxa de um pino é convertida em texto — reutilize, não reimplemente).

- [ ] **Step 4: Verifique no app de verdade**

Isto é UI: os testes não cobrem. Rode o app, e no Production Planner:
1. Crie um Rotor. Ele pede Iron Rod e Screw.
2. Crie um Screw, e ligue um único Iron Rod nos dois — no pino de entrada do Screw e no do Rotor.
3. Confirme que a segunda ligação é **aceita**, que a máquina de Iron Rod **subiu** para a soma, e que o Screw **não** foi achatado.
4. Passe o mouse em cada aresta e confira as duas taxas.
5. Salve, feche, reabra: o fan-out volta com as mesmas taxas.

- [ ] **Step 5: Rode a suíte inteira** — tudo verde.

- [ ] **Step 6: Registre em `PROGRESS.md`** (sem git)

---

### Task 7: GroupNode com fan-out parcial

O `GroupNode` empacota um subgrafo e expõe como pinos externos os pinos internos que ficaram sem link (`group_node.cpp:78-80`, e `GroupSelectedNodes` em `production_app.cpp:319-403`). Surge um estado que antes não existia: **um pino interno com fan-out onde parte dos consumidores ficou fora do grupo** — ele não está nem livre nem totalmente ligado.

Hoje `GroupSelectedNodes` (`:342-348`) simplesmente deleta os links que cruzam a fronteira do grupo, e há um `TODO` no código admitindo isso. Esta tarefa **não** resolve o TODO (manter conexões externas é outra feature). Ela só garante que o fan-out parcial não corrompe o grafo.

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp:319-403` (`GroupSelectedNodes`)
- Modify: `ficsit-companion/src/domain/nodes/group_node.cpp` (`CreateInsOuts`)
- Test: `ficsit-companion/tests/test_group_node.cpp`

**Interfaces:**
- Consumes: tudo das tarefas anteriores.
- Produces: nada novo na API.

- [ ] **Step 1: Escreva o teste que falha**

```cpp
/// @test   Grouping a producer whose fan-out only partly falls inside the selection: the inner
///         branch is preserved within the group, the branch crossing the boundary is cut, and
///         the producer's output pin is left holding exactly one link.
/// @covers GroupSelectedNodes + GroupNode::CreateInsOuts under a partial fan-out. This is the
///         one genuinely new state multi-link introduces into grouping — before, a pin was
///         either wired or not, never half.
TEST_CASE("GroupNode handles a fan-out split across the group boundary", "[group_node][multilink]")
{
    // Build: producer -> consumer_in (40) and producer -> consumer_out (20).
    // Select producer + consumer_in only, then group them.
    // Assert: the producer->consumer_in link survives inside the group;
    //         the producer->consumer_out link is gone from graph.links;
    //         producer->outs[0]->links.size() == 1;
    //         no dangling pointer — the deleted link appears in no Pin::links anywhere.
}
```

O corpo acima está descrito, não escrito: montá-lo exige o caminho de seleção do node editor, que os casos existentes de `test_group_node.cpp` já exercitam. Leia esse arquivo e siga o padrão dele.

- [ ] **Step 2: Rode e confirme que falha**

Esperado: o `process_link` de `GroupSelectedNodes` (`:324-352`) recebe `p->link` — que na Task 1 virou `p->links[0]` ou `SoleLink()`. Com fan-out, ele processa **um** dos links e deixa o outro pendurado apontando para um nó que já saiu de `graph.nodes`. Isso é um ponteiro pendurado, e o teste deve pegá-lo.

- [ ] **Step 3: Implemente**

Em `GroupSelectedNodes`, o laço que chama `process_link` (`:358-365`) deve iterar **todos** os links de cada pino. `process_link` já muta `links`, então itere sobre uma cópia:

```cpp
            for (const auto& p : (*it)->ins)
            {
                const std::vector<Link*> snapshot = p->links;
                for (Link* l : snapshot) process_link(l);
            }
            for (const auto& p : (*it)->outs)
            {
                const std::vector<Link*> snapshot = p->links;
                for (Link* l : snapshot) process_link(l);
            }
```

Em `GroupNode::CreateInsOuts`, o critério "este pino interno vira pino externo do grupo" é hoje "está sem link". Com fan-out parcial isso continua correto **depois** da correção acima, porque os links que cruzam a fronteira são deletados — um pino interno acaba ou sem links (vira pino externo) ou só com links internos. Confirme lendo `CreateInsOuts`; se o critério estiver escrito como `p->link == nullptr`, ele já virou `p->links.empty()` na Task 1 e está certo. Se estiver escrito de outra forma, ajuste.

- [ ] **Step 4: Rode os testes e a suíte** — `[group_node]` PASS, tudo verde.

- [ ] **Step 5: Verifique no app** — monte o fan-out do Rotor, selecione só o Iron Rod e o Screw, agrupe, e confirme que o grupo não deixa aresta órfã na tela nem trava o app.

- [ ] **Step 6: Registre em `PROGRESS.md`** (sem git)

---

## Ordem e dependências

```
Task 1 (modelo)
  └─ Task 2 (solver: variável de link + balanço)
       ├─ Task 2b (rateio proporcional)
       ├─ Task 3 (cadeado não atravessa fan-out)
       │    └─ Task 4 (CreateLink escolhe o pino restringido)
       │         └─ Task 6 (UI: libera, recolore, tooltip)
       ├─ Task 5 (persistência)  [independente de 3/4, precisa de 1 e 2]
       └─ Task 7 (GroupNode)     [depende de 1; melhor depois de 6, para verificar na tela]
```

Tarefas 5 e 7 são independentes entre si e podem ir em paralelo depois da Task 4.
