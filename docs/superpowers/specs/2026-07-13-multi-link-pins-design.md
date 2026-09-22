# Pinos multi-link no Production Planner

**Data:** 2026-07-13
**Status:** aprovado, aguardando plano de implementação

## Problema

No Production Planner, um pino aceita no máximo um link. Para produzir Rotor você precisa
de Iron Rod e Screw, e o Screw também consome Iron Rod. Ligar a mesma máquina de Iron Rod
aos dois consumidores é impossível: a segunda ligação é rejeitada, e o usuário é obrigado a
criar um Splitter à mão só para satisfazer o modelo.

No Factory Snapshot essa restrição é correta — a fábrica importada do `.sav` tem splitters e
mergers como prédios reais, e o grafo deve refletir a topologia do jogo. No planner, ela só
atrapalha: o usuário está desenhando a intenção da produção, não o layout das esteiras.

## Objetivo

Um pino do Production Planner aceita N links. A taxa do pino é a soma das taxas dos seus
links. Vale para pinos de entrada (fan-in, merger implícito) e de saída (fan-out, splitter
implícito).

```
SAÍDA (fan-out)                  ENTRADA (fan-in)
[Rod 60] ─┬─ 40 ─> [Screw]       [Rod A 30] ─┐
          └─ 20 ─> [Rotor]                   ├─> [Rotor 50]
                                 [Rod B 20] ─┘
```

**Fora de escopo:** Factory Snapshot e Vehicle Map continuam com um link por pino.

## Semântica das taxas

Ao adicionar um link a um pino que já tem links, a produção **sobe para atender a soma da
demanda**. Você já tem Iron Rod (40/min) alimentando o Screw; liga o mesmo pino ao Rotor,
que precisa de 20/min; a máquina de Iron Rod passa a 60/min.

## Arquitetura

### Modelo

- `Pin::link` (`Link*`) passa a `Pin::links` (`std::vector<Link*>`).
- `Link` ganha `current_rate` — a taxa daquela aresta. Hoje ela é implícita, porque as duas
  pontas de um link necessariamente têm a mesma taxa.

Invariante: **a taxa de um pino é a soma das taxas dos seus links**. É a mesma equação que
`CustomSplitter` e `Merger` já satisfazem (`soma(ins) = soma(outs)`); um pino multi-link é um
splitter/merger implícito.

### Solver

`RateSolver` monta um sistema linear exato: uma variável por pino (compartilhada entre os
pinos de um mesmo Craft, com coeficiente), e por link a equação `taxa(saída) − taxa(entrada) = 0`.

A generalização: **uma variável nova por link**, e a equação por link é substituída por uma
equação por pino:

```
para cada pino P com links:   soma(taxas dos links de P) − taxa(P) = 0
```

Para um pino de um link só, isso se reduz exatamente à equação atual. Grafos de 1 link se
comportam de forma idêntica — é uma generalização, não uma reescrita.

#### Por que "somar a demanda" não precisa de regra especial

Rod (40) → Screw, e o usuário arrasta o 2º link Rod → Rotor (que precisa de 20). O sistema fica:

```
rotor_in       = 20              (restrição: o novo consumidor manda)
link2          = rotor_in
link1          = screw_in
link1 + link2  = rod_out         (equação nova do pino multi-link)
```

Sobra uma variável livre: a contagem de máquinas do Screw. O solver já resolve variável livre
mantendo o valor atual (`rate_solver_apply.cpp:130`). Screw fica em 40, logo `rod_out = 60`.

A única regra nova está na criação do link, não no solver — ver abaixo.

## Mudanças por arquivo

### Lógica de verdade

**`GraphModel::CreateLink`** — escolha do pino restringido:

- origem já tem links → restringe o **destino** (puxa a demanda);
- destino já tem links → restringe a **origem**;
- nenhum dos dois → comportamento atual.

Hoje o `CreateLink` empurra da origem, o que achataria o Screw para 20 em vez de subir o Rod
para 60.

**`GraphModel::DeleteLink`** — hoje faz `pin->link = nullptr`; passa a remover do vetor. As
limpezas que rodam junto (`RemoveItemIfNotForced` do organizer; zerar item e taxa de Sink e
Logistics) hoje assumem "o pino ficou sem link". Passam a rodar **apenas quando o vetor
esvaziou**.

**`ProductionApp::DragLink`** (`production_app.cpp:2829` e `:2861`) — cai a rejeição por pino
ocupado. É o único ponto onde o planner se separa do snapshot; não é preciso flag de modo,
porque `FactorySnapshotApp` não cria links interativamente (não usa `BeginCreate`/`QueryNewLink`).

**`Pin::SetLocked`** (`pin.cpp:26-33`) — hoje o cadeado atravessa o link: travar um lado trava o
outro. Faz sentido com um link só (as duas pontas têm forçosamente a mesma taxa). Com fan-out,
não: travar a saída do Rod em 60 fixa o **total**, não fixa que o Screw leva 40. O cadeado passa a
atravessar uma aresta **apenas quando ela é o único link dos dois lados**. A mesma guarda vale
para a sincronização de lock em `CreateLink` (`graph_model.cpp:129-133`).

### Mecânico

`Pin::links` toca ~116 pontos em 13 arquivos. A grande maioria é `if (pin->link)` virando
`for (Link* l : pin->links)`. Snapshot, Vehicle Map e `sav_import` continuam corretos sem
tratamento especial: lá os pinos nunca ganham mais de um link, o vetor tem tamanho ≤ 1 e os
laços rodam uma vez.

### Persistência

O link é salvo como `{start: {node, pin}, end: {node, pin}}`. Ganha um campo `rate`.

Saves antigos não têm o campo, e não precisam: num grafo legado todo pino tem ≤ 1 link, logo a
taxa da aresta é a taxa do pino. A ausência do campo é lida assim, e o arquivo antigo abre
idêntico. `GroupNode::Serialize` usa o mesmo formato para o subgrafo; a mudança é a mesma nos
dois lugares.

### UI

`ProductionApp::RenderLinks` pinta a linha de vermelho quando
`link->start->current_rate != link->end->current_rate`. Esse teste deixa de valer: num fan-out
as pontas *devem* diferir. Passa a ser: **vermelho quando a taxa de um pino não bate com a soma
das taxas dos seus links**.

A taxa da aresta aparece no tooltip ao passar o mouse sobre a linha. Isso cobre o único caso em
que o número não é dedutível da tela: um fan-out alimentando um fan-in diretamente, onde nenhum
pino sozinho revela a divisão. Nos casos comuns, o pino de entrada do consumidor (link único) já
mostra a taxa da aresta.

## Risco principal: GroupNode

`GroupNode` empacota um subgrafo e expõe como pinos externos os pinos internos que ficaram sem
link (`group_node.cpp:78-80`). "Sem link" passa a ser "vetor vazio", mas surge um estado que
antes não existia: **um pino interno com fan-out onde parte dos consumidores ficou fora do
grupo** — o pino não está nem livre nem totalmente ligado.

É o único ponto do design onde aparece um estado genuinamente novo, e onde os testes devem se
concentrar.

## Testes

Suíte Catch2 existente. A garantia central: **`test_graph_model`, `test_rate_propagation`,
`test_node_serialization` e `test_session_serializer` devem continuar passando sem alteração** —
é a prova de que a generalização não mexeu no comportamento de 1 link.

Testes novos:

- Fan-out soma a demanda: Rod→Screw a 40, ligar Rod→Rotor (20) leva Rod a 60, Screw fica em 40.
- Fan-in simétrico: dois produtores de Rod num único pino de entrada do Rotor.
- Fan-out com pino de saída travado (locked): a sobra vai para o novo link; demanda acima do
  travado é rejeitada.
- Fan-out alimentando fan-in direto (as duas pontas multi-link): a divisão é determinada pelas
  variáveis de link.
- Deletar um dos N links: os demais sobrevivem; a limpeza de item/taxa só dispara no último.
- Round-trip de serialização com fan-out; e save antigo (sem campo `rate`) abre idêntico.
- GroupNode com fan-out parcialmente interno ao grupo.
